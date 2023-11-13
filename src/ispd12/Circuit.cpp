/*
 *  Circuit.cpp
 *  sizer
 *
 *  Created by Tiago Reimann on 11/01/12.
 *  Copyright 2012 __MyCompanyName__. All rights reserved.
 *
 */

#include "ispd12/Circuit.h"

#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <queue>
#include <sstream>

#include "ispd12/Stepwatch.h"
#include "ispd12/global.h"
#include "ispd12/timer_interface.h"

using std::priority_queue;
using std::queue;
#include <map>
using std::multimap;
#include <functional>
using std::greater;
#include <algorithm>
using std::random_shuffle;
#include <fstream>
using std::fstream;
#include <iomanip>
using std::setw;

bool DEBUG_TIAGO = false;

const double Circuit::EPSILON = 1e-6;

const double Circuit::defaultH = 1;
const double Circuit::defaultCin = 1;

const bool Circuit::enableReport_LogicalEffortDiscretization = true;
const bool Circuit::enableReport_LogicalEffortPropagation = true;
const bool Circuit::enableReport_LogicalEffortCharacterization = true;

// -----------------------------------------------------------------------------

void Circuit::runBaselineFlow() {
  Stepwatch stepwatchFlow("flow");

  cout << "===================================================================="
          "==\n";
  cout << "Baseline Flow\n";
  cout << "===================================================================="
          "==\n";

  cout << "Options:\n";
  cout << "- Ignore leakage: " << optIgnoreLeakagePower << "\n";

  cout << "- Enable lambda-delay sensitivities: "
       << optEnableLambdaDelaySensitivities << "\n";
  cout << "- Enable slack filtering: " << optEnableSlackFiltering << "\n";
  cout << "- Enable gamma: " << optEnableGamma << "\n";
  cout << "- Enable load violation filtering: "
       << optEnableLoadViolationFiltering << "\n";

  cout << "- UseTennakoon: " << optUseTennakoon << "\n";

  cout << "- Run initial sizing: " << optRunInitialSizing << "\n";
  cout << "- Run initial sizing for load and slew violation removal: "
       << optRunInitialSizingForLoadAndSlewViolationRemoval << "\n";
  cout << "- Run power recovery: " << optRunPowerRecovery << "\n";
  cout << "- Run timing recovery: " << optRunTimingRecovery << "\n";

  initialSolution(
      false);  // set initial solution (min leakage cells ou random cells)
  updateTiming();
  printTiming("Initial Solution");

  if (optRunInitialSizingForLoadAndSlewViolationRemoval) {
    Stepwatch stepwatch1("step: sizing for no load violation");
    sizingForNoLoadAndSlewViolationByLiLi();
    // sizingForNoLoadAndSlewViolationByLivramento();
    // sizingForNoLoadViolation();
    // sizingByLiLi();
    stepwatch1.finish();

    updateTiming();
    printTiming("Initial Sizing for Load and Slew Violations Removal");
  }  // end if

  Stepwatch stepwatch2("step: lagrangian relaxation");
  sizingLagrangeRelaxationSensitivities();
  stepwatch2.finish();
  updateTiming();
  printTiming("Lagrangian Relaxation");

  Stepwatch stepwatch3("step: timing recovery");
  if (optRunTimingRecovery) {
    timingRecoveryPathCounter();
  }  // end if
  stepwatch3.finish();

  Stepwatch stepwatch4("step: power recovery");
  if (optRunPowerRecovery) {
    powerRecovery();
  }  // end if
  stepwatch4.finish();

  saveSizes();
  saveSizesDetailed();
  saveSizeTable();

  updateTiming();
  printTiming("Final Solution");

  std::cout << "updateTiming() runtime: "
            << stopwatchUpdateTiming.getElapsedTime() << " s\n";
}  // end method

// -----------------------------------------------------------------------------

void Circuit::calcLoadViol() {
  loadViol = 0;

  for (int i = 0; i < icells.size(); ++i) {
    Vcell *tmpCell = icells[i];

    loadViol +=
        (tmpCell->actualLoad > tmpCell->actualInstType->pins[0].maxCapacitance)
            ? (tmpCell->actualLoad -
               tmpCell->actualInstType->pins[0].maxCapacitance)
            : 0.0;

    // [TODO] WARNING: Assuming that pin[0] is the output!!!.
  }  // end for
  if (DEBUG_TIAGO) cout << " Total Load Violations: " << loadViol << endl;
}

// -----------------------------------------------------------------------------

void Circuit::calcSlewViol() {
  timingViolationSlewVector.assign(timingViolationSlewVector.size(), 0);
  timingViolationSlew = 0;

  const int numNets = timingNets.size();
  for (int i = timingOffsetToNetLevel.front();
       i < timingOffsetToNetLevel.back(); i++) {
    const TimingNet &net = timingNets[i];
    const TimingNetState &netstate = getTimingNetState(i);

    timingViolationSlew +=
        computeSlewViolation(netstate.slew[RISE], net.fanout);
    timingViolationSlew +=
        computeSlewViolation(netstate.slew[FALL], net.fanout);
  }  // end for

  timingViolationSlewVector[0] = timingViolationSlew;
  slewViol = timingViolationSlew;
}  // end method

// -----------------------------------------------------------------------------

void Circuit::calcLeakage() {
  // sum all lekages

  totalLeakage = 0.0;

  for (int i = 0; i < icells.size(); ++i) {
    totalLeakage += icells[i]->actualInstType->leakagePower;
  }
  if (DEBUG_TIAGO) cout << " Total Leakage: " << totalLeakage << endl;
}

// -----------------------------------------------------------------------------

void Circuit::updateCellLoad(Vcell *cell) {
  const int n = cell->sinkNetIndex;
  if (n == -1) return;

  double pinLoad = cell->portLoad;

  const int k0 = timingSinkArcPointers[n];
  const int k1 = timingSinkArcPointers[n + 1];
  for (int k = k0; k < k1; k++) {
    const TimingArc &arc = timingArcs[timingSinkArcs[k]];

    // [TODO] Consider creating a dummy cell to avoid this if.
    if (arc.pin != -1)
      pinLoad += arc.cell->actualInstType->pins[arc.pin].capacitance;
  }  // end for

  TimingNetState &netstate = getTimingNetState(n);
  netstate.load = cell->wireLoad + pinLoad;

  cell->actualLoad = cell->wireLoad + pinLoad;
  cell->outputLoad = pinLoad;
}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateCellTiming(Vcell *cell) {
  cell->cellTiming();
}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateCellType(Vcell *cell, int typeIndex) {
  // cell->indexInc = cell->actualInstTypeIndex - typeIndex;

  assert(!cell->dontTouch);

  loadViol -=
      (cell->actualLoad > cell->actualInstType->pins[0].maxCapacitance)
          ? (cell->actualLoad - cell->actualInstType->pins[0].maxCapacitance)
          : 0.0;
  loadViol = myRound(loadViol, 2);
  totalLeakage -= cell->actualInstType->leakagePower;

  cell->actualInstTypeIndex = typeIndex;
  cell->actualInstType =
      &orgCells.oCells[cell->footprintIndex].cells[cell->actualInstTypeIndex];
  cell->instType = cell->actualInstType->name;

  updateLoads(cell);

  loadViol +=
      (cell->actualLoad > cell->actualInstType->pins[0].maxCapacitance)
          ? (cell->actualLoad - cell->actualInstType->pins[0].maxCapacitance)
          : 0.0;
  loadViol = myRound(loadViol, 2);
  totalLeakage += cell->actualInstType->leakagePower;

  // cout << "Celula escolhida " << cell->instType << " name " << cell->instName
  // << endl;
}  // end method

// -----------------------------------------------------------------------------

bool Circuit::updateCellTypeByLiLi(Vcell *cell) {
  const int n = cell->sinkNetIndex;

  const double originalLoadViolation = loadViol;
  const int originalTypeIndex = cell->actualInstTypeIndex;
  const double originalSizingEffect = computeSizingEffectOnLambdaDelay(n);

  int bestCell = originalTypeIndex;
  double bestSizingEffect = originalSizingEffect;

  const int numCandidateCells =
      orgCells.oCells[cell->footprintIndex].cells.size();
  for (int i = 0; i < numCandidateCells; i++) {
    if (i == originalTypeIndex) continue;

    updateCellType(cell, i);

    // Do not accept load violations.
    if (loadViol > originalLoadViolation) continue;

    updateTimingLocally(n);

    const double sizingEffect = computeSizingEffectOnLambdaDelay(n);
    if (sizingEffect < bestSizingEffect) {
      bestCell = i;
      bestSizingEffect = sizingEffect;
    }  // end if
  }    // end for

  updateCellType(cell, bestCell);
  updateTimingLocally(n);

  return originalTypeIndex != bestCell;
}  // end method

// -----------------------------------------------------------------------------

bool Circuit::updateCellTypeLagrangeRelaxation(Vcell *cell,
                                               const double alpha) {
  const int n = cell->sinkNetIndex;

  const double slackSlack = getSlackSlack(getWorstSlack());

  const double originalLoadViolation = loadViol;
  const int originalTypeIndex = cell->actualInstTypeIndex;
  const double originalSizingEffectOnLambdaDelay =
      computeSizingEffectOnLambdaDelay(n);
  const double originalSizingEffectOnLocalNegativeSlack =
      computeSizingEffectOnLocalNegativeSlack(n);
  const double originalLeakage = cell->getLeakagePower();
  const double originalSlew =
      getTimingNetState(cell->sinkNetIndex).slew.getMax();

  int bestCell = originalTypeIndex;
  double bestCost = originalSizingEffectOnLambdaDelay + alpha * originalLeakage;

  const int numCandidateCells =
      orgCells.oCells[cell->footprintIndex].cells.size();
  for (int i = 0; i < numCandidateCells; i++) {
    if (i == originalTypeIndex) continue;

    updateCellType(cell, i);

    // Do not accept load violations.
    if (loadViol > originalLoadViolation) continue;

    updateTimingLocally(n);

    const double sizingEffectOnLocalNegativeSlack =
        computeSizingEffectOnLocalNegativeSlack(n);
    if ((sizingEffectOnLocalNegativeSlack <
         originalSizingEffectOnLocalNegativeSlack * slackSlack))
      continue;

    const double costLeakage = cell->getLeakagePower();
    const double costSizingEffectOnLambdaDelay =
        computeSizingEffectOnLambdaDelay(n);
    const double cost = costSizingEffectOnLambdaDelay + alpha * costLeakage;

    if (cost < bestCost) {
      bestCell = i;
      bestCost = cost;
    }  // end if
  }    // end for

  updateCellType(cell, bestCell);
  updateTimingLocally(n);

  return originalTypeIndex != bestCell;
}  // end method

// -----------------------------------------------------------------------------

bool Circuit::updateCellTypeLagrangeRelaxationSensitivities(
    Vcell *cell, const double gamma, const double alpha) {
  const int n = cell->sinkNetIndex;

  const double originalLoadViolation = loadViol;
  const int originalTypeIndex = cell->actualInstTypeIndex;
  const double originalSizingEffectOnLambdaDelay =
      computeSizingEffectOnLambdaDelaySensitivities(n);
  const double originalSizingEffectOnLocalNegativeSlack =
      computeSizingEffectOnLocalNegativeSlack(n);
  const double originalLeakage = cell->getLeakagePower();

  int bestCell = originalTypeIndex;
  double bestCost = originalSizingEffectOnLambdaDelay + alpha * originalLeakage;

  const int numCandidateCells =
      orgCells.oCells[cell->footprintIndex].cells.size();
  for (int i = 0; i < numCandidateCells; i++) {
    if (i == originalTypeIndex) continue;

    updateCellType(cell, i);

    // Do not accept load violations.
    if (optEnableLoadViolationFiltering && loadViol > originalLoadViolation)
      continue;

    updateTimingLocallyIncludingSideNets(n);

    const double sizingEffectOnLocalNegativeSlack =
        computeSizingEffectOnLocalNegativeSlack(n);
    if (optEnableSlackFiltering && (sizingEffectOnLocalNegativeSlack <
                                    originalSizingEffectOnLocalNegativeSlack *
                                        (optEnableGamma ? gamma : 1)))
      continue;

    const double costLeakage =
        optIgnoreLeakagePower ? 0 : cell->getLeakagePower();
    const double costSizingEffectOnLambdaDelay =
        computeSizingEffectOnLambdaDelaySensitivities(n);
    const double cost = costSizingEffectOnLambdaDelay + alpha * costLeakage;

    if (cost < bestCost) {
      bestCell = i;
      bestCost = cost;
    }  // end if
  }    // end for

  updateCellType(cell, bestCell);
  updateTimingLocallyIncludingSideNets(n);

  return originalTypeIndex != bestCell;
}  // end method

// -----------------------------------------------------------------------------

bool Circuit::updateCellTypeLagrangeRelaxationSensitivitiesTestingChanges(
    Vcell *cell, const double gamma, const double alpha) {
  const int n = cell->sinkNetIndex;

  const double originalLoadViolation = loadViol;
  const int originalTypeIndex = cell->actualInstTypeIndex;
  const double originalSizingEffectOnLambdaDelay =
      computeSizingEffectOnLambdaDelaySensitivities(n);
  const double originalSizingEffectOnLocalNegativeSlack =
      computeSizingEffectOnLocalNegativeSlack(n);
  const double originalLeakage = cell->getLeakagePower();
  const double originalPathViolation = timingNumPathsWithNegativeSlack;

  int bestCell = originalTypeIndex;
  double bestCost = originalSizingEffectOnLambdaDelay + alpha * originalLeakage;

  const int numCandidateCells =
      orgCells.oCells[cell->footprintIndex].cells.size();
  for (int i = 0; i < numCandidateCells; i++) {
    if (i == originalTypeIndex) continue;

    updateCellType(cell, i);

    double ratio = 0;
    if (worstSlack < 0)
      ratio = timingTotalNegativeSlack / timingNumPathsWithNegativeSlack;

    // Do not accept load violations.
    if (loadViol > originalLoadViolation ||
        (timingNumPathsWithNegativeSlack > 2 * originalPathViolation &&
         worstSlack < 0 && ratio > 3))
      continue;

    updateTimingLocallyIncludingSideNets(n);

    const double sizingEffectOnLocalNegativeSlack =
        computeSizingEffectOnLocalNegativeSlack(n);
    if ((sizingEffectOnLocalNegativeSlack <
         originalSizingEffectOnLocalNegativeSlack * gamma))
      continue;

    const double costLeakage = cell->getLeakagePower();
    const double costSizingEffectOnLambdaDelay =
        computeSizingEffectOnLambdaDelaySensitivities(n);
    const double cost = costSizingEffectOnLambdaDelay + alpha * costLeakage;

    if (cost < bestCost) {
      bestCell = i;
      bestCost = cost;
    }  // end if
  }    // end for

  updateCellType(cell, bestCell);
  updateTimingLocallyIncludingSideNets(n);

  return originalTypeIndex != bestCell;

}  // end method

// -----------------------------------------------------------------------------

bool Circuit::updateCellTypeToReducePowerConsideringLocalSlack(Vcell *cell) {
  const int n = cell->sinkNetIndex;

  const double originalLoadViolation = loadViol;
  const int originalTypeIndex = cell->actualInstTypeIndex;
  const double originalSizingEffectOnDelay =
      computeSizingEffectOnLambdaDelay(n);
  const double originalSizingEffectOnSlack =
      computeSizingEffectOnLocalNegativeSlack(n);
  const double originalLeakage = cell->getLeakagePower();
  const double originalSlew =
      getTimingNetState(cell->sinkNetIndex).slew.getMax();

  int bestCell = originalTypeIndex;
  double bestCost = originalLeakage;

  const int numCandidateCells =
      orgCells.oCells[cell->footprintIndex].cells.size();
  for (int i = 0; i < numCandidateCells; i++) {
    if (i == originalTypeIndex) break;

    updateCellType(cell, i);

    // Do not accept load violations.
    if (loadViol > originalLoadViolation) continue;

    updateTimingLocally(n);

    const double sizingEffectOnSlack =
        computeSizingEffectOnLocalNegativeSlack(n);
    if ((sizingEffectOnSlack < originalSizingEffectOnSlack)) continue;

    // if ( timingNets[cell->sinkNetIndex].slew.getMax() > originalSlew*1.25 )
    // continue;

    const double costLeakage = cell->actualInstType->leakagePower;

    const double cost = costLeakage;

    if (cost < bestCost) {
      bestCell = i;
      bestCost = cost;
    }  // end if
  }    // end for

  updateCellType(cell, bestCell);
  updateTimingLocally(n);

  return originalTypeIndex != bestCell;
}  // end method

// -----------------------------------------------------------------------------

bool Circuit::updateCellTypeConsideringAbsoluteSlack(Vcell *cell) {
  const int n = cell->sinkNetIndex;

  const int originalTypeIndex = cell->actualInstTypeIndex;

  const double originalLoadViolation = loadViol;
  const double originalSizingEffectOnDelay =
      computeSizingEffectOnLambdaDelay(n);
  const double originalSizingEffectOnSlack =
      computeSizingEffectOnAbsoluteSlack(n);

  int bestCell = originalTypeIndex;
  double bestCost = originalSizingEffectOnDelay + originalSizingEffectOnSlack;

  const int numCandidateCells =
      orgCells.oCells[cell->footprintIndex].cells.size();
  for (int i = 0; i < numCandidateCells; i++) {
    if (i == originalTypeIndex) continue;

    updateCellType(cell, i);

    // Do not accept load violations.
    if (loadViol > originalLoadViolation) continue;

    updateTimingLocally(n);

    const double costSizingEffectOnDelay = computeSizingEffectOnLambdaDelay(n);
    const double costSizingEffectOnSlack =
        computeSizingEffectOnAbsoluteSlack(n);

    // if ( timingNets[cell->sinkNetIndex].slew.getMax() > originalSlew*1.25 )
    // continue;

    const double cost = costSizingEffectOnDelay + costSizingEffectOnSlack;

    if (cost < bestCost) {
      bestCell = i;
      bestCost = cost;
    }  // end if
  }    // end for

  updateCellType(cell, bestCell);
  updateTimingLocally(n);

  return originalTypeIndex != bestCell;
}  // end method

// -----------------------------------------------------------------------------

bool Circuit::updateCellTypeToMeetRequiredTime(Vcell *cell) {
  const int n = cell->sinkNetIndex;

  const int originalTypeIndex = cell->actualInstTypeIndex;

  const double originalLoadViolation = loadViol;
  const double originalSizingEffectOnSlack = -computeSizingEffectOnSlack(n);

  int bestCell = originalTypeIndex;
  double bestCost = originalSizingEffectOnSlack;

  const int numCandidateCells =
      orgCells.oCells[cell->footprintIndex].cells.size();
  for (int i = 0; i < numCandidateCells; i++) {
    if (i == originalTypeIndex) continue;

    updateCellType(cell, i);

    // Do not accept load violations.
    if (loadViol > originalLoadViolation) continue;

    updateTimingLocally(n);

    const double sizingEffectOnSlack = -computeSizingEffectOnSlack(n);

    /*
if ((sizingEffectOnSlack < originalSizingEffectOnSlack))
continue;
*/

    const double cost = (sizingEffectOnSlack);

    if (cost < bestCost) {
      bestCell = i;
      bestCost = cost;
    }  // end if
  }    // end for

  updateCellType(cell, bestCell);
  updateTimingLocally(n);

  return originalTypeIndex != bestCell;
}  // end method

// -----------------------------------------------------------------------------

bool Circuit::upsize(Vcell *cell, const bool allowLoadViolation) {
  const int originalTypeIndex = cell->actualInstTypeIndex;
  const double originalLoadViolation = loadViol;

  LibCellGroupCharacterization &option =
      timingLibraryCharacterization[cell->footprintIndex];
  const int vth = option.cellCharacterization[cell->actualInstTypeIndex].vth;
  const int size = option.cellCharacterization[cell->actualInstTypeIndex].size;

  if (size + 1 < option.mappingVthSizeToCellTypeIndex[vth].size()) {
    updateCellType(cell, option.mappingVthSizeToCellTypeIndex[vth][size + 1]);

    if (!allowLoadViolation && (loadViol > originalLoadViolation)) {
      // Roll back.
      updateCellType(cell, originalTypeIndex);
      return false;
    } else {
      return true;
    }  // end else
  } else {
    return false;
  }  // end else
}  // end method

// -----------------------------------------------------------------------------

bool Circuit::downsize(Vcell *cell, const bool allowLoadViolation) {
  const int originalTypeIndex = cell->actualInstTypeIndex;
  const double originalLoadViolation = loadViol;

  LibCellGroupCharacterization &option =
      timingLibraryCharacterization[cell->footprintIndex];
  const int vth = option.cellCharacterization[cell->actualInstTypeIndex].vth;
  const int size = option.cellCharacterization[cell->actualInstTypeIndex].size;

  if (size - 1 >= 0) {
    updateCellType(cell, option.mappingVthSizeToCellTypeIndex[vth][size - 1]);

    if (!allowLoadViolation && (loadViol > originalLoadViolation)) {
      // Roll back.
      updateCellType(cell, originalTypeIndex);
      return false;
    } else {
      return true;
    }  // end else
  } else {
    return false;
  }  // end else
}  // end method

// -----------------------------------------------------------------------------

bool Circuit::increaseVth(Vcell *cell, const bool allowLoadViolation) {
  const int originalTypeIndex = cell->actualInstTypeIndex;
  const double originalLoadViolation = loadViol;

  LibCellGroupCharacterization &option =
      timingLibraryCharacterization[cell->footprintIndex];
  const int vth = option.cellCharacterization[cell->actualInstTypeIndex].vth;
  const int size = option.cellCharacterization[cell->actualInstTypeIndex].size;

  if (vth - 1 >= 0) {
    updateCellType(cell, option.mappingVthSizeToCellTypeIndex[vth - 1][size]);

    if (!allowLoadViolation && (loadViol > originalLoadViolation)) {
      // Roll back.
      updateCellType(cell, originalTypeIndex);
      return false;
    } else {
      return true;
    }  // end else
  } else {
    return false;
  }  // end else
}  // end method

// -----------------------------------------------------------------------------

bool Circuit::decreaseVth(Vcell *cell, const bool allowLoadViolation) {
  const int originalTypeIndex = cell->actualInstTypeIndex;
  const double originalLoadViolation = loadViol;

  LibCellGroupCharacterization &option =
      timingLibraryCharacterization[cell->footprintIndex];
  const int vth = option.cellCharacterization[cell->actualInstTypeIndex].vth;
  const int size = option.cellCharacterization[cell->actualInstTypeIndex].size;

  if (vth + 1 < option.mappingVthSizeToCellTypeIndex.size()) {
    updateCellType(cell, option.mappingVthSizeToCellTypeIndex[vth + 1][size]);

    if (!allowLoadViolation && (loadViol > originalLoadViolation)) {
      // Roll back.
      updateCellType(cell, originalTypeIndex);
      return false;
    } else {
      return true;
    }  // end else
  } else {
    return false;
  }  // end else
}  // end method

// -----------------------------------------------------------------------------

void Circuit::downsizeSinks(const int n) {
  const int k0 = timingSinkNets[n];
  const int k1 = timingSinkNets[n + 1];
  for (int k = k0; k < k1; k++) {
    Vcell *cell = timingNets[timingSinkNets[k]].driver;
    downsize(cell);
  }  // end method
}  // end method

// -----------------------------------------------------------------------------

// bool Circuit::reducingLeakageByLiLi() {
//     int changedCounter = 0;
//
//	for (int i=offsetCombinational; i < depthSortedCells.size(); i++) {
//		Vcell *cell = depthSortedCells[i];
//
//		if ( cell->dontTouch)
//			continue;
//
//		bool changed = false;
//
//		const int n = cell->sinkNetIndex;
//		updateTimingLocally(n);
//
//		const double originalLoadViolation = loadViol;
//		const int originalTypeIndex = cell->actualInstTypeIndex;
//		const int originalPower = cell->actualInstType->leakagePower;
//		const double originalSizingEffect =
// computeSizingEffectOnDelayWithoutLambda(n); 		double originalSlack =
// timingTotalNegativeSlack;
//
//		int bestCell = originalTypeIndex;
//		double bestSizingEffect = originalSizingEffect;
//		double deltaDelay = 0.0;
//		double deltaPower = 0.0;
//		double initialLeakage = totalLeakage;
//		double sensitivity = 0;
//		double bestSensitivity = -numeric_limits<double>::max();
//
//		//cout << "CELL " << cell->instName << endl;
//
//		//////////// PROCESS DELAY VALUES BEFORE TEST CANDIDATES CELLS
// DELAY EFFECTS /////////////////////////
//
//		// Process previous and current level timing arcs.
//
//
//		const int k0 = timingLocalArcPointers[n];
//		const int k1 = timingLocalArcPointers[n + 1];
//		for (int k = k0; k < k1; k++) {
//			TimingArc &arc = timingArcs[timingLocalArcs[k]];
//			arc.pastDelay = arc.delay;
//		} // end for
//
//		const int numCandidateCells =
// orgCells.oCells[cell->footprintIndex].cells.size(); 		for ( int i = 0;
// i < originalTypeIndex; i++ ) {
//
//			if (i == originalTypeIndex)
//				continue;
//
//			updateCellType(cell,i);
//			//updateTiming();
//
//			// Do not accept load violations.
//			if ( loadViol > originalLoadViolation || totalLeakage >
// initialLeakage) 				continue;
//
//			updateTiming(cell);
//
//			if ( timingTotalNegativeSlack > originalSlack )
//				continue;
//
//			const double deltaDelay =
// computeSizingEffectOnSlackLiLi(n);
//
//			//const double sizingEffect =
// computeSizingEffectOnDelayWithoutLambda(n);
//			//cout << "Cell " << cell->instName << " tipo que era "
//<< originalTypeIndex << " tipo agora " << i << endl;
//			//cout << "Original effect " << originalSizingEffect <<
//" efeito atual " << sizingEffect << endl;
//			//deltaDelay = max(-originalSizingEffect,
//-sizingEffect); 			deltaPower =  originalPower -
// cell->actualInstType->leakagePower; 			sensitivity =
// deltaPower/deltaDelay;
//
//			//cout << "Delta power \t" << deltaPower << " delta
// delay \t" << deltaDelay << " sensitivity \t" << sensitivity << endl;
//
//			if ( sensitivity > bestSensitivity){
//				bestCell = i;
//				bestSensitivity = sensitivity;
//				initialLeakage = totalLeakage;
//				originalSlack = timingTotalNegativeSlack;
//				changed = true;
//				//break;
//			} // end if
//		} // end for
//		if ( changed )
//			changedCounter++;
//
//		updateCellType(cell, bestCell);
//		updateTiming();
//		//if ( n%1000 == 0)
//		//printTimingDigest();
//	} // end for
//
//	return changedCounter!=0;
//
// } // end method

// -----------------------------------------------------------------------------

void Circuit::calcLoads() {
  // calc cells output load (wire + downstream + port capacitance)

  queue<Vcell *> qCircuit;
  Vcell *tmpCell;
  string outputNet, inputPin;

  //	cout << " Calculating cells outputs loads.." << endl;
  for (int i = 0; i < graphRoot.nextCells.size(); ++i) {
    tmpCell = graphRoot.nextCells[i];

    tmpCell->changed = false;
    updateCellLoad(tmpCell);
  }

  for (int i = 0; i < icells.size(); ++i) {
    tmpCell = icells[i];
    tmpCell->changed = false;

    updateCellLoad(tmpCell);
  }

  //	cout << " Calculating cells outputs loads..done" << endl;
}

void Circuit::updateLoads(Vcell *changedCell) {
  // calc cells output loads for a changed cell (wire + downstream + port
  // capacitance)

  Vcell *tmpCell;
  string outputNet, inputPin;

  //	cout << " Calculating cells outputs loads.." << endl;
  for (int a = 0; a < changedCell->previousCells.size(); ++a) {
    tmpCell = changedCell->previousCells[a];

    loadViol -=
        (tmpCell->actualLoad > tmpCell->actualInstType->pins[0].maxCapacitance)
            ? (tmpCell->actualLoad -
               tmpCell->actualInstType->pins[0].maxCapacitance)
            : 0.0;
    updateCellLoad(tmpCell);
    loadViol +=
        (tmpCell->actualLoad > tmpCell->actualInstType->pins[0].maxCapacitance)
            ? (tmpCell->actualLoad -
               tmpCell->actualInstType->pins[0].maxCapacitance)
            : 0.0;
  }
}

void Circuit::printCells() {
  for (int i = 0; i < orgCells.oCells[1].cells.size(); ++i)
    cout << orgCells.oCells[1].cells[i].name << "\t"
         << orgCells.oCells[1].cells[i].leakagePower << "\t"
         << orgCells.oCells[1].cells[i].pins[0].maxCapacitance << "\t[" << i
         << "]" << endl;
}

void Circuit::criticalImprovement(const int temp) {
  debug("tiago", "critical improvement...");
  updateTopCriticalPaths(5 + min(5, timingNumPathsWithNegativeSlack / 100));
  // if (temp < 0.1)
  for (int i = this->topCriticalCells.size(); i >= 0; --i) {
    simulatedAnnealing(temp, true);
    if (this->timingViol == 0.0) break;
  }
}

int Circuit::changeCellsReimann(const double temp) {
  /*
   if ( (getSize() > 1e5) && (temp < 2e-6) )//&& (rand() % 100 <= ratio/10.0))
   {
   int iterations = parallelAnnealing(temp, false);
   return iterations;
   }
       else */
  {
    // simulatedAnnealing(temp, false);
    simulatedAnnealingTestedWithUFSC(temp, false);
    return 1;
  }
}

int Circuit::changeCellsFastTimingSA(const double temp) {
  //	const int circuitSize = getSize();
  int iterations = 1;

  // if ((temp > 1e-6)) cout << "akiT1 ";
  if (worstSlack <= -0.005) {
    // if ((temp > 1e-6)) cout << "akiT0 ";
    // debug("tiago2", "critical ");
    updateTopCriticalPaths(min(2, timingNumPathsWithNegativeSlack));
    criticalIterator = topCriticalCells.begin();
    // if ((temp > 1e-6)) cout << "akiT1 " << topCriticalCells.size()*3 << " ";
    for (int i = 100 /*topCriticalCells.size()*3*/; i >= 0;
         --i)  // int(100.0/max(temp,2.5e-1))
    {
      // simulatedAnnealing(temp/10.0, true);
      // simulatedAnnealing(temp, true);
      simulatedAnnealingTestedWithUFSC(temp, true);

      // iterations += parallelAnnealing(temp/10.0, true);
      /*
       if (worstSlack > -0.005)
       break;*/
    }
  }

  return iterations;
  // totalCost = timingViol*alpha + slewViol*alpha + loadViol*beta +
  // totalLeakage; debug("tiago", " Current cost: " << totalCost << "\t" <<
  // loadViol << "\t" << totalLeakage << "\t" << timingViol << " (" << minViol
  // << ")" << "\t" << slewViol << "\t" << worstSlack << endl); debug("tiago", "
  // Best cost (no viol.): " << bestCost << endl);
}

void Circuit::changeCellsFastLoadSA(const double temp) {
  // where the actual sizing algorithm must be!

  if ((loadViol > 0.0)  //&&
                        // ( (rand() % circuitSize/(1.0+loadViol)) == 0) &&
                        // (timingViol == 0.0)
  ) {
    debug("tiago2", "load2 ");
    updateTopLoadPaths();
    criticalIterator = topCriticalCells.begin();
    for (int i = min(5000, int(loadViol) * 5); i >= 0; --i) {
      simulatedAnnealing(temp / 100.0, true);
      // parallelAnnealing(temp/100.0, true);

      if (loadViol == 0.0) break;
    }
  }
  /*
totalCost = timingViol*alpha + slewViol*alpha + loadViol*beta + totalLeakage;
debug("tiago", " Current cost: " << totalCost << "\t" << loadViol << "\t" <<
totalLeakage << "\t" << timingViol << " (" << minViol << ")" << "\t" << slewViol
<< "\t" << worstSlack << endl);
//debug("tiago", " Best cost (no viol.): " << bestCost << endl);

updateTiming_WorstArrivalTime();
loadViol = myRound(loadViol, 2);
slewViol = myRound(timingViolationSlew, 2);

timingViol = myRound(timingViolationSlack, 2);
worstSlack = sdcInfos.clk_period - timingWorstArrivalTime.getMax();
minViol = min(minViol, myRound(timingViolationSlack, 2));
*/
}

void Circuit::changeCellsFastLoadSAUsedUFSC(const double temp) {
  // where the actual sizing algorithm must be!
  double lastCost, totalCost = 0.0, alpha, beta;
  alpha = 1.0 * (1.0 / temp);
  beta = 1.0 * (1.0 / (temp * temp));
  const int circuitSize = this->getSize();

  if ((this->loadViol > 0.0)  //&&
                              // ( (rand() % circuitSize/(1.0+this->loadViol))
                              // == 0) && (this->timingViol == 0.0)
  ) {
    debug("tiago2", "load2 ");
    updateTopLoadPaths();
    criticalIterator = topCriticalCells.begin();
    for (int i = min(5000, int(this->loadViol) * 5); i >= 0; --i) {
      simulatedAnnealing(temp / 100.0, true);
      // parallelAnnealing(temp/100.0, true);

      if (this->loadViol == 0.0) break;
    }
  }

  cout << "Load 2 " << endl;
  /*
totalCost = this->timingViol*alpha + this->slewViol*alpha + this->loadViol*beta
+ this->totalLeakage; debug("tiago", " Current cost: " << totalCost << "\t" <<
this->loadViol << "\t" << this->totalLeakage << "\t" << this->timingViol << " ("
<< this->minViol << ")" << "\t" << this->slewViol << "\t" << this->worstSlack <<
endl);
//debug("tiago", " Best cost (no viol.): " << this->bestCost << endl);
/*
updateTiming_WorstArrivalTime();
this->loadViol = myRound(this->loadViol, 2);
this->slewViol = myRound(this->timingViolationSlew, 2);

this->timingViol = myRound(this->timingViolationSlack, 2);
this->worstSlack = this->sdcInfos.clk_period -
this->timingWorstArrivalTime.getMax(); this->minViol = min(this->minViol,
myRound(this->timingViolationSlack, 2));
*/
}

void Circuit::changeCellsFastLeakageSA(const double temp) {
  debug("tiago", "non-critical ");
  updateLeastCriticalPaths(1 + pathTails.size());
  // if (temp < 0.1)
  for (int i = topCriticalCells.size(); i >= 0; --i) {
    simulatedAnnealing(temp, true);
  }
  /*
simulatedAnnealing(temp, false);

totalCost = timingViol*alpha + slewViol*alpha + loadViol*beta + totalLeakage;
debug("tiago", " Current cost: " << totalCost << "\t" << loadViol << "\t" <<
totalLeakage << "\t" << timingViol << " (" << minViol << ")" << "\t" << slewViol
<< "\t" << worstSlack << endl); debug("tiago", " Best cost (no viol.): " <<
bestCost << endl);
*/
}

void Circuit::simulatedAnnealing(const long double temp, bool critical) {
  queue<Vcell *> qCircuit;
  int newIndex = 0;
  long double lastCost, totalCost = 0.0;
  const long unsigned int ref = RAND_MAX;
  double beta, alpha;

  if (temp > 1.0e5) {
    beta = 1.0 * (1.0 / temp);
    alpha = 1.0 * (1.0 / (temp * temp));
  } else {
    alpha = 1.0 * (1.0 / temp);
    beta = 10.0 * (1.0 / (temp * temp));
  }
  Vcell *chosenCell = NULL;

  const int topSize = topCriticalCells.size();
  const int totalSize = icells.size();

  int trials = 0;
  // if ((critical) && (temp > 1e-6)) cout << "aki0 ";
  if ((critical) && (topSize > 0)) do {
      chosenCell = *criticalIterator;  // topCriticalCells[rand() % topSize];
      ++criticalIterator;
      if (criticalIterator == topCriticalCells.end())
        criticalIterator = topCriticalCells.begin();

      // if ((critical) && (temp > 1e-6)) cout << "aki01 ";
    } while ((chosenCell->dontTouch));
  else
    do { /*
      if ( (chosenCell != NULL) && (chosenCell->trials > 0) &&
      (chosenCell->changes/chosenCell->trials < 0.001)) {
      --chosenCell->trials;
      ++cellIterator;
      if(cellIterator == depthSortedCells.end())
      cellIterator = depthSortedCells.begin()+offsetCombinational;
      }*/

      ++trials;
      chosenCell = *cellIterator;  // icells[rand() % totalSize];
      ++cellIterator;

      if (cellIterator == depthSortedCells.end())
        cellIterator = depthSortedCells.begin();

    } while ((chosenCell->dontTouch)
             // || ((chosenCell->trials > 0) &&
             // (chosenCell->changes/chosenCell->trials < 0.001))
             ||
             ((worstSlack > -0.005) && (temp < 2.0e-6) &&
              (chosenCell->actualInstTypeIndex <= 1) && (trials < totalSize)));

  // if ((critical) && (temp > 1e-6)) cout << "aki1 ";
  const int numCell =
      30;  // orgCells.oCells[chosenCell->footprintIndex].cells.size();
  const int lastIndex = chosenCell->actualInstTypeIndex;

  do {
    newIndex = rand() % numCell;
  } while ((newIndex == lastIndex) || (((newIndex - lastIndex) > 6)) ||
           (critical && ((newIndex - lastIndex) > 3))
           //|| ( (worstSlack > -0.005) && (fabs(newIndex - lastIndex) > 10) &&
           //(temp < 2e-6) && (lastIndex != 0) && (newIndex - lastIndex) > 0)
           //|| ((!critical) && (lastIndex != 0) && (worstSlack > -0.005) &&
           //((newIndex - lastIndex) > 0))
  );
  //|| ((timingViol == 0.0) && (newIndex - lastIndex > 0)));//|| ( (newIndex -
  // lastIndex) > ceil(temp+2.0) ) );

  // if ( critical ) debug("tiago2", " last index: " << lastIndex << " new
  // index: " << newIndex << endl);

  /*   Vcell * teste = *cellIterator;
   cout << teste->instType << endl;
   char nada;cin >> nada;
   ++cellIterator;
   */
  loadViol = myRound(loadViol, 2);
  slewViol = myRound(timingViolationSlew, 2);

  const double lastTV = timingViol = myRound(timingTotalNegativeSlack, 2);
  worstSlack =
      myRound(sdcInfos.clk_period - timingWorstArrivalTime.getMax(), 2);
  const double lastSlack = worstSlack;
  minViol = min(minViol, myRound(timingTotalNegativeSlack, 2));

  const double myClock = getClkPeriod();
  lastCost =
      ((worstSlack > -0.0050) ? 0.0 : timingViol) * alpha + slewViol * alpha +
      loadViol * beta + totalLeakage +
      exp(fabs((worstSlack > -0.0050) ? 0.0 : worstSlack) / (myClock * 0.025)) *
          alpha;  // pow(fabs(min(0.0,worstSlack)),1.0)*beta;
  /*
   const long double lastLeakage = totalLeakage;
   const long double lastSlew = slewViol;
   const long double lastTiming = timingViol;
   const long double lastLoad = loadViol;
   */
  // if ( critical ) debug("tiago2", " Current costs: " << lastCost << "\t" <<
  // loadViol << "\t" << totalLeakage << "\t" << timingViol << " (" << minViol
  // << ")" << "\t" << slewViol << "\t" << worstSlack << endl);//"\t" <<
  // sdcInfos.clk_period*0.1*temp << endl; debug("tiago2", " cell: " <<
  // chosenCell->instName << " changed from: " << chosenCell->instType);

  updateCellType(chosenCell, newIndex);

  // if ( critical )debug("tiago2", " to: " << chosenCell->instType << endl);

  updateTiming(chosenCell);

  loadViol = myRound(loadViol, 2);
  slewViol = myRound(timingViolationSlew, 2);

  timingViol = myRound(timingTotalNegativeSlack, 2);
  worstSlack =
      myRound(sdcInfos.clk_period - timingWorstArrivalTime.getMax(), 2);
  minViol = min(minViol, myRound(timingTotalNegativeSlack, 2));
  // if ( critical ) debug("tiago2", " Current costs: " << lastCost << "\t" <<
  // loadViol << "\t" << totalLeakage << "\t" << timingViol << " (" << minViol
  // << ")" << "\t" << slewViol << "\t" << worstSlack << endl);//"\t" <<
  // sdcInfos.clk_period*0.1*temp << endl;

  totalCost =
      ((worstSlack > -0.0050) ? 0.0 : timingViol) * alpha + slewViol * alpha +
      loadViol * beta + totalLeakage +
      exp(fabs((worstSlack > -0.0050) ? 0.0 : worstSlack) / (myClock * 0.025)) *
          alpha;  // pow(fabs(min(0.0,worstSlack)),1.0)*beta;
  // if ( critical ) debug("tiago2", " Current costs: " << totalCost << "\t" <<
  // loadViol << "\t" << totalLeakage << "\t" << timingViol << " (" << minViol
  // << ")" << "\t" << slewViol << "\t" << worstSlack << endl);//"\t" <<
  // sdcInfos.clk_period*0.1*temp << endl;
  /*
   if ( critical ){
   debug("tiago2", "leakage: " << lastLeakage - totalLeakage << "\t" <<
   lastTiming - timingViol << "\t" << lastSlew - slewViol << "\t"<< lastLoad -
   loadViol << "\t"<< lastCost-totalCost  << "\t"<< totalCost  << "\t"<<
   lastCost << endl );

   debug("tiago2", " Current cost: " << totalCost << "\t" << loadViol << "\t" <<
   totalLeakage << "\t" << timingViol << " (" << minViol << ")" << "\t" <<
   slewViol << "\t" << worstSlack << endl);//"\t" <<
   sdcInfos.clk_period*0.1*temp << endl; debug("tiago2", totalCost - lastCost <<
   endl);
   }*/

  /*
   if ( (temp > 2e-6) && (bestCost*0.999 > totalLeakage) && (worstSlack >
   -0.005) && (slewViol <= 0.0) && (loadViol <= 0.0) )
   {
   storeBestSolution();
   bestCost = totalLeakage;
   maxLeakage = totalLeakage;
   }
   */
  long double delta = lastCost - totalCost;
  /*
   if (delta == 0.0)
   delta = lastLeakage-totalLeakage;
   */
  const long double boltz = exp(delta / temp);
  const long unsigned int factor = (long unsigned int)(ref * boltz);

  ++accepts;

  int undo = 0;
  undo += (delta < 0) ? 1 : 0;
  undo +=
      ((this->timingViol > this->maxTimingViol) && (this->timingViol > lastTV))
          ? 1
          : 0;
  undo += ((this->worstSlack < this->maxWorstSlack) &&
           (this->worstSlack < lastSlack))
              ? 1
              : 0;
  //      undo += (this->worstSlack < -1.0+lastSlack)?1:0;
  undo = (rand() % ref > ref * exp((lastCost - totalCost) / (temp))) ? undo : 0;
  undo += ((this->worstSlack < this->maxWorstSlack) &&
           (this->worstSlack < lastSlack))
              ? 1
              : 0;
  undo += (this->worstSlack < min(-0.1, lastSlack) - 1.0) ? 1 : 0;

  if (undo > 0) {
    // if (critical) debug("tiago2", "UNDOING... ("<<factor<<")
    // "<<lastCost-totalCost<<endl);
    /*  debug("tiago1", "\nUNDOING... ("<<totalCost<<")
     * ("<<lastCost<<")"<<endl);
     */
    updateCellType(chosenCell, lastIndex);

    updateTiming(chosenCell);

    ++rejects;
    --accepts;

    loadViol = myRound(loadViol, 2);
    slewViol = myRound(timingViolationSlew, 2);

    timingViol = myRound(timingTotalNegativeSlack, 2);
    worstSlack =
        myRound(sdcInfos.clk_period - timingWorstArrivalTime.getMax(), 2);
    minViol = min(minViol, myRound(timingTotalNegativeSlack, 2));

    // totalCost = ((worstSlack > -0.0050)?0.0:timingViol)*alpha +
    // slewViol*alpha + loadViol*beta + totalLeakage + exp(fabs((worstSlack >
    // -0.0050)?0.0:worstSlack)/(myClock*0.01))*alpha;//pow(fabs(min(0.0,worstSlack)),1.0)*beta;
  }
  /*
   debug("tiago1", " Best cost (no viol.): " << bestCost << endl);

   debug("tiago1", "Changing cells...done" << endl);*/
  // if (temp == 0.1) {char nada; cin >> nada;}
}

void Circuit::simulatedAnnealingTestedWithUFSC(const long double temp,
                                               bool critical) {
  queue<Vcell *> qCircuit;
  int newIndex = 0;
  long double lastCost, totalCost = 0.0;
  const long unsigned int ref = RAND_MAX;
  double beta, alpha;

  if (temp > 1.0e5) {
    beta = 1.0 * (1.0 / temp);
    alpha = 1.0 * (1.0 / (temp * temp));
  } else {
    alpha = 1.0 * (1.0 / temp);
    beta = 10.0 * (1.0 / (temp * temp));
  }
  Vcell *chosenCell = NULL;

  const int topSize = this->topCriticalCells.size();
  const int totalSize = this->icells.size();

  int trials = 0;
  // if ((critical) && (temp > 1e-6)) cout << "aki0 ";
  if ((critical) && (topSize > 0)) do {
      chosenCell =
          *criticalIterator;  // this->topCriticalCells[rand() % topSize];
      ++criticalIterator;
      if (criticalIterator == topCriticalCells.end())
        criticalIterator = topCriticalCells.begin();

      // if ((critical) && (temp > 1e-6)) cout << "aki01 ";
    } while ((chosenCell->dontTouch));
  else
    do { /*
      if ( (chosenCell != NULL) && (chosenCell->trials > 0) &&
      (chosenCell->changes/chosenCell->trials < 0.001)) {
      --chosenCell->trials;
      ++cellIterator;
      if(cellIterator == depthSortedCells.end())
      cellIterator = depthSortedCells.begin()+offsetCombinational;
      }*/

      ++trials;
      chosenCell = *cellIterator;  // this->icells[rand() % totalSize];
      ++cellIterator;

      if (cellIterator == depthSortedCells.end())
        cellIterator = depthSortedCells.begin();

    } while ((chosenCell->dontTouch)
             // || ((chosenCell->trials > 0) &&
             // (chosenCell->changes/chosenCell->trials < 0.001))
             ||
             ((this->worstSlack > -0.005) && (temp < 2.0e-6) &&
              (chosenCell->actualInstTypeIndex <= 1) && (trials < totalSize)));

  // if ((critical) && (temp > 1e-6)) cout << "aki1 ";
  const int numCell =
      30;  // orgCells.oCells[chosenCell->footprintIndex].cells.size();
  const int lastIndex = chosenCell->actualInstTypeIndex;

  do {
    newIndex = rand() % numCell;
  } while ((newIndex == lastIndex) ||
           (((newIndex - lastIndex) > 3))
           //|| ( critical && ((newIndex - lastIndex) > 3) )
           ||
           ((this->worstSlack > -0.005) && (fabs(newIndex - lastIndex) > 10) &&
            (temp < 2e-6) && (lastIndex != 0) && (newIndex - lastIndex) > 0) ||
           ((!critical) && (lastIndex != 0) && (this->worstSlack > -0.005) &&
            ((newIndex - lastIndex) > 0)));
  //|| ((this->timingViol == 0.0) && (newIndex - lastIndex > 0)));//|| (
  //(newIndex - lastIndex) > ceil(temp+2.0) ) );

  // if ( critical ) debug("tiago2", " last index: " << lastIndex << " new
  // index: " << newIndex << endl);

  /*   Vcell * teste = *cellIterator;
   cout << teste->instType << endl;
   char nada;cin >> nada;
   ++cellIterator;
   */
  this->loadViol = myRound(this->loadViol, 2);
  this->slewViol = myRound(this->timingViolationSlew, 2);

  const double lastTV = this->timingViol =
      myRound(this->timingTotalNegativeSlack, 2);
  this->worstSlack = myRound(
      this->sdcInfos.clk_period - this->timingWorstArrivalTime.getMax(), 2);
  this->minViol =
      min(this->minViol, myRound(this->timingTotalNegativeSlack, 2));

  const double myClock = getClkPeriod();
  lastCost = ((this->worstSlack > -0.0050) ? 0.0 : this->timingViol) * alpha +
             this->slewViol * alpha + this->loadViol * beta +
             this->totalLeakage +
             exp(fabs((this->worstSlack > -0.0050) ? 0.0 : this->worstSlack) /
                 (myClock * 0.025)) *
                 alpha;  // pow(fabs(min(0.0,this->worstSlack)),1.0)*beta;
  const long double lastLeakage = this->totalLeakage;
  const long double lastSlew = slewViol;
  const long double lastTiming = timingViol;
  const long double lastLoad = loadViol;

  // if ( critical ) debug("tiago2", " Current costs: " << lastCost << "\t" <<
  // this->loadViol << "\t" << this->totalLeakage << "\t" << this->timingViol <<
  // " (" << this->minViol << ")" << "\t" << this->slewViol << "\t" <<
  // this->worstSlack << endl);//"\t" << sdcInfos.clk_period*0.1*temp << endl;
  // debug("tiago2", " cell: " << chosenCell->instName << " changed from: " <<
  // chosenCell->instType);

  updateCellType(chosenCell, newIndex);

  // if ( critical )debug("tiago2", " to: " << chosenCell->instType << endl);

  updateTiming(chosenCell);

  this->loadViol = myRound(this->loadViol, 2);
  this->slewViol = myRound(this->timingViolationSlew, 2);

  this->timingViol = myRound(this->timingTotalNegativeSlack, 2);
  this->worstSlack = myRound(
      this->sdcInfos.clk_period - this->timingWorstArrivalTime.getMax(), 2);
  this->minViol =
      min(this->minViol, myRound(this->timingTotalNegativeSlack, 2));
  // if ( critical ) debug("tiago2", " Current costs: " << lastCost << "\t" <<
  // this->loadViol << "\t" << this->totalLeakage << "\t" << this->timingViol <<
  // " (" << this->minViol << ")" << "\t" << this->slewViol << "\t" <<
  // this->worstSlack << endl);//"\t" << sdcInfos.clk_period*0.1*temp << endl;

  totalCost = ((this->worstSlack > -0.0050) ? 0.0 : this->timingViol) * alpha +
              this->slewViol * alpha + this->loadViol * beta +
              this->totalLeakage +
              exp(fabs((this->worstSlack > -0.0050) ? 0.0 : this->worstSlack) /
                  (myClock * 0.025)) *
                  alpha;  // pow(fabs(min(0.0,this->worstSlack)),1.0)*beta;
  // if ( critical ) debug("tiago2", " Current costs: " << totalCost << "\t" <<
  // this->loadViol << "\t" << this->totalLeakage << "\t" << this->timingViol <<
  // " (" << this->minViol << ")" << "\t" << this->slewViol << "\t" <<
  // this->worstSlack << endl);//"\t" << sdcInfos.clk_period*0.1*temp << endl;
  /*
   if ( critical ){
   debug("tiago2", "leakage: " << lastLeakage - totalLeakage << "\t" <<
   lastTiming - timingViol << "\t" << lastSlew - slewViol << "\t"<< lastLoad -
   loadViol << "\t"<< lastCost-totalCost  << "\t"<< totalCost  << "\t"<<
   lastCost << endl );

   debug("tiago2", " Current cost: " << totalCost << "\t" << this->loadViol <<
   "\t" << this->totalLeakage << "\t" << this->timingViol << " (" <<
   this->minViol << ")" << "\t" << this->slewViol << "\t" << this->worstSlack <<
   endl);//"\t" << sdcInfos.clk_period*0.1*temp << endl; debug("tiago2",
   totalCost - lastCost << endl);
   }*/
  if ((temp > 2e-6) && (this->bestCost * 0.999 > this->totalLeakage) &&
      (this->worstSlack > -0.005) && (this->slewViol <= 0.0) &&
      (this->loadViol <= 0.0)) {
    storeBestSolution();
    this->bestCost = this->totalLeakage;
    this->maxLeakage = this->totalLeakage;
  }

  long double delta = lastCost - totalCost;
  /*
   if (delta == 0.0)
   delta = lastLeakage-this->totalLeakage;
   */
  const long double boltz = exp(delta / temp);
  const long unsigned int factor = (long unsigned int)(ref * boltz);

  ++accepts;
  ++acceptsChanges;

  /*  if (boltz == HUGE_VALL)
   cout << "huge" <<endl;


   if ( (!critical) && (lastLeakage < totalLeakage))
   debug("tiago4", "delta: " << delta  << " boltz: " << boltz  << " factor: " <<
   factor  << endl );
   */
  if ((delta < 0.0) && (rand() % ref > factor)
      //|| (boltz == HUGE_VALL)
      //|| (totalCost-lastCost > 1.0e6)
      ||
      ((this->timingViol > this->maxTimingViol) && (this->timingViol > lastTV))
      //|| ((this->totalLeakage > this->maxLeakage) && (this->totalLeakage >
      // lastL))
  ) {
    // if (critical) debug("tiago2", "UNDOING... ("<<factor<<")
    // "<<lastCost-totalCost<<endl);
    /*  debug("tiago1", "\nUNDOING... ("<<totalCost<<")
     * ("<<lastCost<<")"<<endl);
     */
    updateCellType(chosenCell, lastIndex);

    updateTiming(chosenCell);

    ++rejects;
    ++rejectsChanges;
    --accepts;
    --acceptsChanges;

    this->loadViol = myRound(this->loadViol, 2);
    this->slewViol = myRound(this->timingViolationSlew, 2);

    this->timingViol = myRound(this->timingTotalNegativeSlack, 2);
    this->worstSlack = myRound(
        this->sdcInfos.clk_period - this->timingWorstArrivalTime.getMax(), 2);
    this->minViol =
        min(this->minViol, myRound(this->timingTotalNegativeSlack, 2));

    // totalCost = ((this->worstSlack > -0.0050)?0.0:this->timingViol)*alpha +
    // this->slewViol*alpha + this->loadViol*beta + this->totalLeakage +
    // exp(fabs((this->worstSlack >
    // -0.0050)?0.0:this->worstSlack)/(myClock*0.01))*alpha;//pow(fabs(min(0.0,this->worstSlack)),1.0)*beta;
  }
  /*
   debug("tiago1", " Best cost (no viol.): " << this->bestCost << endl);

   debug("tiago1", "Changing cells...done" << endl);*/
  // if (temp == 0.1) {char nada; cin >> nada;}
}

void Circuit::setNewCellSize(Vcell *tmpCell) {
  // find cell without max load violation

  int count = 0;

  while ((count < 3) && (tmpCell->actualLoad >
                         tmpCell->actualInstType->pins[0].maxCapacitance)) {
    ++count;
    if ((tmpCell->dontTouch) ||
        (tmpCell->actualInstTypeIndex >=
         orgCells.oCells[tmpCell->footprintIndex].cells.size() -
             1)) {  // reduz todas as nextCells possiveis
      for (int i = 0; i < tmpCell->nextCells.size(); ++i)
        if (tmpCell->nextCells[i]->actualInstTypeIndex > 0) {
          updateCellType(tmpCell->nextCells[i],
                         tmpCell->nextCells[i]->actualInstTypeIndex - 1);
        }
    } else {
      updateCellType(tmpCell, tmpCell->actualInstTypeIndex + 1);
    }
  }
}

void Circuit::solveLoadViol() {
  // solve (try) max load violations

  queue<Vcell *> qCircuit;
  Vcell *tmpCell;
  int changeCounter = 0, visited = 0, lastChange;

  // calcLoads();

  // if (DEBUG_TIAGO) cout << " Solving maxload violations..." << endl;
  for (int i = 0; i < this->icells.size(); ++i) {
    this->icells[i]->changed = false;
  }
  for (int i = 0; i < graphRoot.nextCells.size(); ++i) {
    graphRoot.nextCells[i]->changed = false;
    qCircuit.push(graphRoot.nextCells[i]);
  }

  while (!qCircuit.empty()) {
    tmpCell = qCircuit.front();
    qCircuit.pop();
    ++visited;
    if (/*(!tmpCell->dontTouch) && */ (
        tmpCell->actualLoad >
        tmpCell->actualInstType->pins[0].maxCapacitance)) {
      setNewCellSize(tmpCell);
      // tmpCell->indexInc = 0;
      ++changeCounter;
    }

    for (int i = 0; i < tmpCell->nextCells.size(); ++i)
      if (!tmpCell->nextCells[i]->changed) {
        qCircuit.push(tmpCell->nextCells[i]);
        tmpCell->nextCells[i]->changed = true;
      }
  }
  // cout<<" Cells changed due to maxload viol.: " << changeCounter << " (" <<
  // visited << " cells)" << endl; if (DEBUG_TIAGO) cout << " Solving maxload
  // violations...done" << endl;

  // calcLoads();

  // if (changeCounter > this->icells.size()/100)
  //	solveLoadViol();
}

void Circuit::initialSolution(bool random) {
  // select min leakage cells for all instances

  queue<Vcell *> qCircuit;
  Vcell *tmpCell;
  LibParserCellInfo *tmp1Cell;
  int i, j, aux;
  set<Vcell>::iterator it;

  cout << "\nSetting initial solution..." << endl;
  cout << " Assigning min size cells..." << endl;
  // find min leakage without testing max_load

  int countCells = 0, notSizeable = 0;

  for (i = 0; i < icells.size(); ++i) {
    tmpCell = icells[i];

    if (optRunInitialSizing) {
      for (j = 0; j < orgCells.oCells.size(); ++j) {
        if (orgCells.oCells[j].footprint.compare(
                0, orgCells.oCells[j].footprint.size(), tmpCell->instType, 0,
                orgCells.oCells[j].footprint.size()) == 0) {
          if (random)
            aux = rand() %
                  orgCells.oCells[j]
                      .cells
                      .size();  // +
                                // orgCells.oCells[j].cells.size()/2;//rand()
                                // % orgCells[j].cells.size();
          else
            aux = 0;  // orgCells.oCells[j].cells.size()/2;//0;
          tmp1Cell = &orgCells.oCells[j].cells[aux];
          // cout << " Cell footprint: " << orgCells.oCells[j].footprint <<
          // endl;
          break;
        }  // end if
      }    // end for
    } else {
      tmp1Cell = tmpCell->actualInstType;
      j = tmpCell->footprintIndex;
      aux = tmpCell->actualInstTypeIndex;
    }  // end else

    // tmpCell->clkPeriod = sdcInfos.clk_period;
    tmpCell->actualInstTypeIndex = aux;
    tmpCell->footprintIndex = j;
    tmpCell->instType = tmp1Cell->name;
    tmpCell->actualInstType = tmp1Cell;
    tmpCell->dontTouch =
        ((tmp1Cell->isSequential) || (tmp1Cell->dontTouch)) ? true : false;

    if (tmp1Cell->isSequential) {
      qCircuit.push(tmpCell);
      tmpCell->changed = true;
      ++countCells;
      ++notSizeable;
    }
    // updateCellType(tmpCell, 0);
    // cout << " Cell name: " << tmpCell->instType << endl;
    // cout << " Cell footprint: " << orgCells[j].footprint << "(" <<
    // tmpCell->dontTouch << ")" << endl;
  }
  cout << " Assigning min size cells...done" << endl;

  cout << " Building circuit graph..." << endl;
  // build circuit graph
  for (int i = 0; i < graphRoot.nextCells.size(); ++i) {
    qCircuit.push(graphRoot.nextCells[i]);
    graphRoot.nextCells[i]->changed = true;
    ++countCells;
  }

  map<string, Net>::iterator itn;
  Net tmpNet;
  set<Vcell *>::iterator itc;
  Vcell *aCell;

  while (!qCircuit.empty()) {
    tmpCell = qCircuit.front();
    qCircuit.pop();

    // int endpointCounter = 0;
    for (int a = tmpCell->pinNetPairs.size() - 1; a >= 0; --a) {
      if (tmpCell->pinNetPairs[a].first == "o") {
        assert(tmpCell->nextCells.size() == 0);
        tmpNet.netName = tmpCell->pinNetPairs[a].second;
        itn = nets.find(tmpNet.netName);
        tmpNet = itn->second;
        for (itc = tmpNet.cells->begin(); itc != tmpNet.cells->end(); ++itc) {
          aCell = *itc;
          if ((aCell->instName != tmpCell->instName)) {
            tmpCell->nextCells.push_back(aCell);
            /*if ((aCell->dontTouch) || (aCell->portLoad != 0.0))
++endpointCounter;*/
            aCell->previousCells.push_back(tmpCell);
            if (!aCell->changed) {
              qCircuit.push(aCell);
              aCell->changed = true;
              ++countCells;
            }
          }
        }
        /*
if (endpointCounter > 1)
cout << " multi endpoint fanout! " << endl;
*/
        tmpCell->nextNum = tmpNet.numPins - 1;  // -1 discount the output pin

        break;
      }
    }  // end for
  }

  for (int i = 0; i < graphRoot.nextCells.size(); ++i) {
    //		graphRoot.nextCells[i]->prevNum =
    // graphRoot.nextCells[i]->previousCells.size();
    graphRoot.nextCells[i]->nextNum = graphRoot.nextCells[i]->nextCells.size();
  }
  for (i = 0; i < icells.size(); ++i) {
    tmpCell = icells[i];

    if ((tmpCell->actualInstType->isSequential) &&
        (tmpCell->previousCells.size() == 0)) {
      tmpCell->nextCells.push_back(tmpCell);
      tmpCell->previousCells.push_back(tmpCell);
    }

    // tmpCell->prevNum = tmpCell->previousCells.size();
  }

  sensitivityOffsetInputSlew = 1.0;
  sensitivityOffsetOutputLoad = 1.0;

  cout << " Total graph cell count: " << countCells << " ("
       << countCells - graphRoot.nextCells.size() << " cells + "
       << graphRoot.nextCells.size() << " inputs)" << endl;
  cout << " Sizeable cell count: "
       << countCells - graphRoot.nextCells.size() - notSizeable << endl;
  cout << " Building circuit graph...done" << endl;

  cout << "Setting initial solution...done\n" << endl;

  cout << "Computing path boundaries...\n";
  computePathBoundaries();
  cout << "Computing path boundaries...done\n" << endl;

  cout << "Computing cell depths...\n";
  computeCellDepths();  // [NOTE] It depends on computePathBoundaries().
  cout << "Computing cell depths...done\n" << endl;

  cout << "Computing cell reverse depths...\n";
  computeCellReverseDepths();  // [NOTE] It depends on computeCellDepths().
  cout << "Computing cell reverse depths...done\n" << endl;

  cout << "Computing average number of sinks...\n";
  computeAvgNumberOfSinks();
  cout << "Computing average number of sinks...done\n" << endl;

  cout << "Building timing structure...\n";
  buildTimingStructure();
  cout << "Building timing structure...done\n" << endl;

  cout << "Computing local nets...\n";
  computeLocalNets();
  cout << "Computing local nets...done\n" << endl;

  cout << "Computing local arcs...\n";
  computeLocalArcs();
  cout << "Computing local arcs...done\n" << endl;

  cout << "Computing cell type table...\n";
  computeCellSizingOptions();
  cout << "Computing cell type table...done\n" << endl;

  cout << "Computing number o path passing thru nets...\n";
  computeNumberOfPathsPassingTrhuNets();
  cout << "Computing number o path passing thru nets... done\n" << endl;

  cout << "Computing weighted number o path passing thru nets...\n";
  computeWeightedNumberOfPathsPassingTrhuNets();
  cout << "Computing weighted number o path passing thru nets... done\n"
       << endl;

  cout << "Computing logical effort...\n";
  computeLogicalEffort();
  cout << "Computing logical effort... done\n" << endl;

  /*
   cout << "Computing pseudo-independent net sets...\n";
   computePseudoIndependentSets(); // [NOTE] It depends on
   buildTimingStructure(). cout << "Computing pseudo-independent net
   sets......done\n" << endl;
   */

  /*
   cout << "Computing expected delays...\n";
   computeExpectedDelays(); // [NOTE] It depends on computeCellDepths() and
   buildTimingStructure(). cout << "Computing expected delays...done\n" << endl;
   */

  /*
   cout << "Computing spans...\n";
   computeSpans();
   cout << "Computing spans...done\n" << endl;

   cout << "Computing reverse spans...\n";
   computeReverseSpans();
   cout << "Computing reverse spans...done\n" << endl;
   */

#ifdef PARALLEL
  setupMultithreading();
#else
  timingViolationSlewVector.resize(1, 0);
#endif

  calcLoads();
  calcLeakage();

  updateTiming();

  calcLoadViol();
  calcSlewViol();

  printTiming("Initial");

  if (enableReport_LogicalEffortDiscretization)
    reportFile_LogicalEffortDiscretization.open(
        "report-logical-effort-discretization.dat");

  if (enableReport_LogicalEffortCharacterization)
    reportFile_LogicalEffortCharacterization.open(
        "report-logical-effort-chaterization.dat");

  analysis_FanoutDistribution();

#ifndef NDEBUG
  // Check if some cell has more than one output as our code does not support
  // multi-output for now;
  for (int i = 0; i < icells.size(); i++) {
    Vcell *cell = icells[i];

    int outputCounter = 0;

    const vector<LibParserPinInfo> &pins = cell->actualInstType->pins;
    for (int k = 0; k < pins.size(); k++) {
      const LibParserPinInfo &pininfo = cell->actualInstType->pins[k];
      if (!pininfo.isInput) outputCounter++;
    }  // end for

    if (outputCounter == 0) {
      cerr << "[BUG] @ Circuit::initialSolution() - "
           << "Cell " << cell->instType << " has no output pin.\n";
    }  // end if

    if (outputCounter > 1) {
      cerr << "[BUG] @ Circuit::initialSolution() - "
           << "Cell " << cell->instType
           << " has multiple output pins. [NOT SUPPORTED].\n";
    }  // end if
  }    // end for

  // Check if sequential cells have more than one previous cells.
  for (int i = 0; i < icells.size(); i++) {
    Vcell *cell = icells[i];
    if (cell->actualInstType->isSequential &&
        (!cell->previousCells.size() > 1)) {
      cerr << "[BUG] @ Circuit::initialSolution() - "
           << "Sequential cell " << cell->instType
           << " has multiple previous cells.\n";
    }  // end if
  }    // end for

#endif

}  // end method

vector<pair<string, string>> Circuit::copySizes() {
  // copy cell sizes to call PrimeTime

  vector<pair<string, string>> tmp;
  set<Vcell *>::iterator it;
  Vcell *tmpCell;

  for (int i = 0; i < icells.size(); ++i) {
    tmpCell = icells[i];
    tmp.push_back(make_pair(tmpCell->instName, tmpCell->instType));
  }

  return tmp;
}

// -----------------------------------------------------------------------------

void Circuit::saveSizes() {
  // save sizes to .sizes file

  FILE *pfile;
  string filename = rootDir + "/" + benchName + "/" + benchName + ".sizes";

  if (pfile = fopen(filename.c_str(), "w")) {
    cout << " Saving sizes..." << endl;
    set<Vcell *>::iterator it;
    Vcell *tmpCell;

    for (int i = 0; i < icells.size(); ++i) {
      tmpCell = icells[i];
      fprintf(pfile, "%s %s\n", tmpCell->instName.c_str(),
              tmpCell->instType.c_str());
    }
    fclose(pfile);
    cout << " Saving sizes...done" << endl;
  } else
    cerr << "Problem saving .sizes file!" << endl;
}

// -----------------------------------------------------------------------------

void Circuit::saveSizesDetailed() {
  // save sizes to .sizes file

  FILE *pfile;
  string filename = benchName + ".xsizes";

  if (pfile = fopen(filename.c_str(), "w")) {
    cout << " Saving sizes with details (only sizable cells)..." << endl;
    set<Vcell *>::iterator it;
    Vcell *tmpCell;

    for (int i = 0; i < icells.size(); ++i) {
      tmpCell = icells[i];
      if (tmpCell->dontTouch) continue;

      const LibCellCharacterization &cellChar =
          getLibCellCharacterization(tmpCell);

      TimingNet &net = timingNets[tmpCell->sinkNetIndex];

      fprintf(pfile, "%s %s size=%d vth=%d fanout=%d depth=%d paths=%d\n",
              tmpCell->instName.c_str(), tmpCell->instType.c_str(),
              cellChar.size, cellChar.vth, (int)tmpCell->nextCells.size(),
              tmpCell->logicalDepth, (net.ipaths * net.opaths));
    }
    fclose(pfile);
    cout << " Saving sizes with details...done" << endl;
  } else
    cerr << "Problem saving .sizes file!" << endl;
}

// -----------------------------------------------------------------------------

void Circuit::saveSizeTable() {
  // save sizes to .sizes file

  FILE *pfile;
  string filename = benchName + ".tsizes";

  std::vector<std::vector<int>> table;
  table.resize(10);
  for (int i = 0; i < 10; i++) table[i].resize(3, 0);

  for (int i = 0; i < icells.size(); ++i) {
    Vcell *tmpCell = icells[i];
    if (tmpCell->dontTouch) continue;

    const LibCellCharacterization &cellChar =
        getLibCellCharacterization(tmpCell);

    table[cellChar.size][cellChar.vth]++;
  }  // end if

  if (pfile = fopen(filename.c_str(), "w")) {
    cout << " Saving size table..." << endl;

    for (int size = 0; size < 10; size++) {
      fprintf(pfile, "%d %d %d\n", table[size][0], table[size][1],
              table[size][2]);
    }  // end for

    fclose(pfile);
    cout << " Saving size table...done" << endl;
  } else {
    std::cerr << "Problem saving .sizes file!" << endl;
  }  // end else
}  // end else

// -----------------------------------------------------------------------------

void Circuit::saveSolution(const string &name) {
  const string solutionName = benchName + "-" + name + ".sizes";

  cout << "Saving solution... ";

  ofstream file(solutionName.c_str());
  if (!file) {
    cout << "Unable to create the file. Aborting!\n";
    exit(1);
  }  // end if

  for (int i = offsetSequential; i < depthSortedCells.size(); i++) {
    Vcell *cell = depthSortedCells[i];
    file << cell->instName << " " << cell->instType << "\n";
  }  // end for
  cout << "Done!\n";
}  // end method

// -----------------------------------------------------------------------------

void Circuit::readSolution(const string &filename) {
  cout << "Reading solution... ";

  ifstream file(filename.c_str());
  if (!file) {
    cout << "Unable to open the file. Aborting!\n";
    exit(1);
  }  // end if

  string instName;
  string instType;

  map<string, string> m;

  while (true) {
    file >> instName >> instType;

    if (file.eof()) break;

    if (file.fail()) {
      cout << "Parse error. Aborting!\n";
      exit(1);
    }  // end if

    m[instName] = instType;
  }  // end while

  for (int i = offsetCombinational; i < depthSortedCells.size(); ++i) {
    Vcell *cell = depthSortedCells[i];

    map<string, string>::iterator it = m.find(cell->instName);
    if (it == m.end()) {
      cout << "Size for instance '" << cell->instName
           << "' not defined. Aborting!\n";
      exit(1);
    }  // end if

    const string instType = it->second;

    bool found = false;

    const int numCandidateCells =
        orgCells.oCells[cell->footprintIndex].cells.size();
    for (int k = 0; k < numCandidateCells; k++) {
      const LibParserCellInfo &info =
          orgCells.oCells[cell->footprintIndex].cells[k];

      if (info.name == instType) {
        updateCellType(cell, k);
        found = true;
        break;
      }  // end if
    }    // end for

    if (!found) {
      cout << "Invalid size '" << instType << "'. Aborting!\n";
      exit(1);
    }  // end if
  }    // end for

  cout << "Done!\n";

  updateTiming();
  printTiming();
}  // end method

// -----------------------------------------------------------------------------

void Circuit::saveHistogramPathSlacks(const int iteration) {
  ostringstream oss;
  oss << "path-slack-histogram-" << benchName << "-" << iteration;

  cout << "Saving path slack histogram... ";

  ofstream script("script.gnuplot");

  script << "\n"
         << "reset"
         << "\n"
         << "n=1000 #number of intervals"
         << "\n"
         << "max=3.	#max value"
         << "\n"
         << "min=-3. #min value"
         << "\n"
         << "width=(max-min)/n	#interval width"
         << "\n"
         << "#function used to map a value to the intervals"
         << "\n"
         << "hist(x,width)=width*floor(x/width)+width/2.0"
         << "\n"
         << "set term png #output terminal and file"
         << "\n"
         << "set output \"" << oss.str() << ".png\""
         << "\n"
         << "set xrange [" << (-getT() / 1) << ":" << getT() << "]"
         << "\n"
         << "set yrange [0:200]"
         << "\n"
         << "#to put an empty boundary around the"
         << "\n"
         << "#data inside an autoscaled graph."
         << "\n"
         << "set offset graph 0.05,0.05,0.05,0.0"
         << "\n"
         << "#set xtics min,(max-min)/5,max"
         << "\n"
         << "set boxwidth width*0.9"
         << "\n"
         << "set style fill solid 0.5	#fillstyle"
         << "\n"
         << "set tics out nomirror"
         << "\n"
         << "set xlabel \"Clock\""
         << "\n"
         << "set ylabel \"Frequency\""
         << "\n"
         << "#set logscale y"
         << "\n"
         << "#count and plot"
         << "\n"
         << "plot \"data.dat\" u (hist($1,width)):(1.0) smooth freq w boxes lc "
            "rgb\"blue\" notitle";
  script.flush();
  script.close();

  ofstream file("data.dat");
  if (!file) {
    cout << "Unable to create the file. Aborting!\n";
    exit(1);
  }  // end if

  const int numPaths = pathTails.size();
  for (int i = 0; i < numPaths; i++) {
    Vcell *cell = pathTails[i];
    const EdgeArray<double> slack = getNetSlack(cell->sinkNetIndex);

    file << slack[RISE] << "\n";
    file << slack[FALL] << "\n";
  }  // end for
  file.flush();
  file.close();

  system("gnuplot script.gnuplot");

  //	std::FILE *f = popen("gnuplot", "w");
  //	std::fputs(script.str().c_str(), f);
  //	std::fflush(f);

  cout << "Done!\n";
}  // end method

// -----------------------------------------------------------------------------

void Circuit::saveHistogramLambdas(const int iteration) {
  ostringstream oss;
  oss << "lambda-histogram-" << benchName << "-" << iteration;

  cout << "Saving lambda histogram... ";

  ofstream script("script.gnuplot");

  script << "\n"
         << "reset"
         << "\n"
         << "n=1000 #number of intervals"
         << "\n"
         << "max=3.	#max value"
         << "\n"
         << "min=-3. #min value"
         << "\n"
         << "width=(max-min)/n	#interval width"
         << "\n"
         << "#function used to map a value to the intervals"
         << "\n"
         << "hist(x,width)=width*floor(x/width)+width/2.0"
         << "\n"
         << "set term png #output terminal and file"
         << "\n"
         << "set output \"" << oss.str() << ".png\""
         << "\n"
         << "set xrange [0:100]"
         << "\n"
         << "set yrange [0:100]"
         << "\n"
         << "#to put an empty boundary around the"
         << "\n"
         << "#data inside an autoscaled graph."
         << "\n"
         << "set offset graph 0.05,0.05,0.05,0.0"
         << "\n"
         << "#set xtics min,(max-min)/5,max"
         << "\n"
         << "set boxwidth width*0.9"
         << "\n"
         << "set style fill solid 0.5	#fillstyle"
         << "\n"
         << "set tics out nomirror"
         << "\n"
         << "set xlabel \"Lambda\""
         << "\n"
         << "set ylabel \"Frequency\""
         << "\n"
         << "#set logscale y"
         << "\n"
         << "#count and plot"
         << "\n"
         << "plot \"data.dat\" u (hist($1,width)):(1.0) smooth freq w boxes lc "
            "rgb\"red\" notitle";
  script.flush();
  script.close();

  ofstream file("data.dat");
  if (!file) {
    cout << "Unable to create the file. Aborting!\n";
    exit(1);
  }  // end if

  const int numArcs = timingArcs.size();
  for (int i = timingOffsetToCombinationArcs; i < numArcs; i++) {
    Vcell *cell = pathTails[i];
    const EdgeArray<double> lambda = getTimingArcState(i).lambda;

    file << lambda[RISE] << "\n";
    file << lambda[FALL] << "\n";
  }  // end for
  file.flush();
  file.close();

  system("gnuplot script.gnuplot");

  //	std::FILE *f = popen("gnuplot", "w");
  //	std::fputs(script.str().c_str(), f);
  //	std::fflush(f);

  cout << "Done!\n";
}  // end method

// -----------------------------------------------------------------------------

void Circuit::sortCells() {
  // build vector of sorted cells according to their footprint

  bool found = false;
  OrdCells new_fcell;
  orgCells.oCells.clear();
  cout << "Associating cells..." << endl;
  for (int i = 0; i < lib_cells.size(); ++i) {
    found = false;
    for (int j = 0; j < orgCells.oCells.size(); ++j)
      if (lib_cells[i].footprint == orgCells.oCells[j].footprint) {
        orgCells.oCells[j].cells.push_back(lib_cells[i]);
        found = true;
        break;
      }

    if (!found) {
      new_fcell.footprint = lib_cells[i].footprint;
      new_fcell.cells.clear();
      new_fcell.cells.push_back(lib_cells[i]);
      orgCells.oCells.push_back(new_fcell);
    }
  }
  for (int i = 0; i < orgCells.oCells.size(); ++i) {
    make_heap(orgCells.oCells[i].cells.begin(), orgCells.oCells[i].cells.end(),
              comp_cellLeakage());
    sort_heap(orgCells.oCells[i].cells.begin(), orgCells.oCells[i].cells.end(),
              comp_cellLeakage());
  }
  cout << " Number of different cell footprints: " << orgCells.oCells.size()
       << endl;
  /*
for (int j = 0; j<orgCells.oCells.size(); ++j)
for (int i = 0; i<orgCells.oCells[j].cells.size(); ++i)
cout << orgCells.oCells[j].cells[i].name << " " <<
orgCells.oCells[j].cells[i].leakagePower << endl;

char nada;cin >> nada;*/
  cout << "Associating cells...done" << endl << endl;
}

void Circuit::readLib() {
  // read cell library

  LibParser lp(rootDir + "/lib/contest.lib");

  bool valid = lp.read_default_max_transition(maxTransition);

  lib_cells.clear();
  assert(valid);
  cout << " The default max transition defined is " << maxTransition << endl;

  int readCnt = 0;
  do {
    LibParserCellInfo cell;
    valid = lp.read_cell_info(cell);

    if (valid) {
      ++readCnt;
      // cout << cell << endl ;
      lib_cells.push_back(cell);
      // cout << lib_cells.back() << endl ;
    }

  } while (valid);

  cout << " Read " << readCnt << " library cells (" << lib_cells.size() << ")"
       << endl;
}

void Circuit::readVerilog() {
  // read circuit verilog
  string filename = rootDir + "/" + benchName + "/" + benchName + ".v";

  VerilogParser vp(filename);

  string moduleName;
  bool valid = vp.read_module(moduleName);
  // cout << "Module " << moduleName << endl << endl ;
  assert(valid);
  Vcell *tmpCell;
  Wire tmp_wire;

  // cout << "Module " << moduleName << endl << endl ;

  string primaryInput;
  do {
    valid = vp.read_primary_input(primaryInput);

    if (valid) {
      // cout << "Primary input: " << primaryInput << endl ;
      inputs.push_back(primaryInput);
    }
  } while (valid);

  // cout << endl ;

  string primaryOutput;
  do {
    valid = vp.read_primary_output(primaryOutput);

    if (valid) {
      // cout << "Primary output: " << primaryOutput << endl ;
      // thisCircuit.outputs.push_back(primaryOutput);
    }

  } while (valid);

  // cout << endl ;

  string net;
  do {
    valid = vp.read_wire(net);

    if (valid) {
      // cout << "Net: " << net << endl ;
      tmp_wire.wire_name = net;
      tmp_wire.cap = 0.0;
      wires.insert(tmp_wire);
    }
  } while (valid);

  // cout << endl ;
  // cout << "Cell insts: " << std::endl ;
  map<string, Net>::iterator it;
  // char nada;
  AddrCell celladdr;

  string cellType, cellInst;
  vector<std::pair<string, string>> pinNetPairs;
  set<string> nets;
  do {
    valid = vp.read_cell_inst(cellType, cellInst, pinNetPairs);

    if (valid) {
      // cout << cellType << " " << cellInst << " " <<endl;
      tmpCell = new Vcell;
      tmpCell->instName = cellInst;
      tmpCell->instType = cellType;
      tmpCell->pinNetPairs.clear();
      nets.clear();

      for (int i = 0; i < pinNetPairs.size(); ++i) {
        // cout << "(" << pinNetPairs[i].first << " " << pinNetPairs[i].second
        // << ") " << endl;
        tmpCell->pinNetPairs.push_back(pinNetPairs[i]);
        tmpCell->inputSlews.push_back(make_pair(0.0, 0.0));
        tmpCell->delays.push_back(make_pair(0.0, 0.0));
        // tmpCell->arrivalTimes.push_back(make_pair(0.0,0.0));

        Net tmpNet;
        tmpNet.cells = new set<Vcell *>;
        tmpNet.cells->clear();
        tmpNet.netName = pinNetPairs[i].second;
        it = this->nets.find(tmpNet.netName);
        if (it != this->nets.end()) {
          tmpNet.cells = it->second.cells;
          tmpNet.cells->insert(tmpCell);
          it->second.numPins++;
          //	nets.erase(it);
        } else {
          tmpNet.cells->insert(tmpCell);
          tmpNet.numPins = 1;
          this->nets.insert(make_pair(tmpNet.netName, tmpNet));
        }
        // it = nets.begin();
        // tmpNet = *it;
        // cout<<thisCircuit.nets.size()<<" "<<tmpNet.netName<<endl;
        // cin >> nada;
      }
      tmpCell->vectorIndex = icells.size();
      icells.push_back(tmpCell);

      /* needed if using PT
       celladdr.vectorIndex = icells.size() - 1;
       celladdr.instName = tmpCell->instName;
       icells_addr.insert(celladdr);
       /* */
      // cout << " Cell: " << celladdr.instName << " is at index: " <<
      // celladdr.vectorIndex << endl; cout << endl ;
    }

  } while (valid);
  cout << " Cell instances read: " << icells.size() << endl;
}

void Circuit::readSDC() {
  // read .sdc file
  string filename = rootDir + "/" + benchName + "/" + benchName + ".sdc";

  SdcParser sp(filename);

  string clockName;
  string clockPort;
  double period;
  bool valid = sp.read_clock(clockName, clockPort, period);
  InputDelay inp_delay;
  InputDriver inp_driver;
  OutputLoad out_load;
  OutputDelay out_delay;
  Vcell *tmpCell;

  assert(valid);
  // cout << "Clock " << clockName << " connected to port " << clockPort  << "
  // has period " << period << endl ;

  sdcInfos.clk_name = clockName;
  sdcInfos.clk_port = clockPort;
  sdcInfos.clk_period = period;

  // create root
  graphRoot.instName = "root";
  graphRoot.instType = "root";

  do {
    string portName;
    double delay;

    valid = sp.read_input_delay(portName, delay);

    if (valid) {
      // cout << "Input port " << portName << " has delay " << delay << endl ;
      inp_delay.port_name = portName;
      inp_delay.delay = delay;
      sdcInfos.inputDelays.push_back(inp_delay);
    }

  } while (valid);

  do {
    string portName;
    string driverSize;
    string driverPin;
    double inputTransitionFall;
    double inputTransitionRise;

    valid = sp.read_driver_info(portName, driverSize, driverPin,
                                inputTransitionFall, inputTransitionRise);

    if (valid) {
      // cout << "Input port " << portName << " is assumed to be connected to
      // the " << driverPin << " pin of lib cell " << driverSize << endl ; cout
      // << "This virtual driver is assumed to have input transitions: " <<
      // inputTransitionFall << " (fall) and " << inputTransitionRise << "
      // (rise)" << endl ;
      inp_driver.port_name = portName;
      inp_driver.driver = driverSize;
      inp_driver.rise = inputTransitionRise;
      inp_driver.fall = inputTransitionFall;
      sdcInfos.input_drivers.push_back(inp_driver);

      tmpCell = new Vcell;
      tmpCell->instName = "inputDriver";
      tmpCell->instType = driverSize;

      tmpCell->actualInstType = orgCells.findCellInst(driverSize);
      // [BUG FIX] After ISVLSI 2013 (PhD Thesis) - does not affect the ISVLSI
      // results
      tmpCell->footprintIndex =
          orgCells.findFootprintIndex(tmpCell->actualInstType->footprint);
      tmpCell->actualInstTypeIndex =
          orgCells.oCells[tmpCell->footprintIndex].findCellTypeIndex(
              driverSize);
      // [BUG FIX] After ISVLSI 2013 (PhD Thesis)
      tmpCell->pinNetPairs.push_back(
          make_pair(tmpCell->actualInstType->pins[1].name, portName));
      tmpCell->pinNetPairs.push_back(make_pair(driverPin, portName));

      tmpCell->inputSlews.push_back(
          make_pair(inputTransitionRise, inputTransitionFall));
      tmpCell->delays.push_back(make_pair(0.0, 0.0));
      /*			tmpPTI.ok = true;
       tmpPTI.rise = inputTransitionRise;
       tmpPTI.fall = inputTransitionFall;
       tmpCell->pinTimingOK.push_back(tmpPTI);
       */
      tmpCell->dontTouch = true;
      tmpCell->pinOk = 0;
      /*
       for (int i = 0; i < sdcInfos.inputDelays.size(); ++i)
       if (sdcInfos.inputDelays[i].port_name == portName) {
       // [TODO] need to read to timing structure: DONE!!
       //tmpCell->arrivalTimes.push_back(make_pair(sdcInfos.inputDelays[i].delay,sdcInfos.inputDelays[i].delay));
       }
       */
      graphRoot.nextCells.push_back(tmpCell);
    }

  } while (valid);

  do {
    string portName;
    double delay;

    valid = sp.read_output_delay(portName, delay);

    if (valid) {
      // cout << "Output port " << portName << " has delay " << delay << endl ;
      out_delay.port_name = portName;
      out_delay.delay = delay;
      sdcInfos.output_delays.push_back(out_delay);
    }
  } while (valid);

  set<Vcell *>::iterator it;

  do {
    string portName;
    double load;

    valid = sp.read_output_load(portName, load);

    if (valid) {
      // cout << "Output port " << portName << " has load " << load << endl ;
      out_load.port_name = portName;
      out_load.load = load;
      sdcInfos.output_loads.push_back(out_load);

      // save load in cell structure
      for (int i = 0; i < icells.size(); ++i) {
        tmpCell = icells[i];
        for (int k = tmpCell->pinNetPairs.size() - 1; k >= 0; --k)
          if ((tmpCell->pinNetPairs[k].first == "o") &&
              (tmpCell->pinNetPairs[k].second == portName)) {
            // cout << tmpCell->instName << " " << tmpCell->pinNetPairs[k].first
            // << " " << tmpCell->pinNetPairs[k].second << endl;
            tmpCell->portLoad = load;
            outputs.push_back(make_pair(portName, tmpCell));
            //	cout << thisCircuit.icells[j]->outputLoad << endl;
            break;
          }
      }
    }
  } while (valid);

  cout << " SDC input delays: " << sdcInfos.inputDelays.size() << endl;
  cout << " SDC input drivers: " << sdcInfos.input_drivers.size() << endl;
  cout << " SDC output delays: " << sdcInfos.output_delays.size() << endl;
  cout << " SDC output loads: " << sdcInfos.output_loads.size() << endl;

  // Increment nextNum of outputs nets.
  for (int i = 0; i < outputs.size(); i++) {
    map<string, Net>::iterator it = nets.find(outputs[i].first);
    assert(it != nets.end());
    it->second.numPins++;
  }  // end for

  // printTree(&graphRoot);
}

void Circuit::readSPEF() {
  // read .spef file
  string filename = rootDir + "/" + benchName + "/" + benchName + ".spef";

  SpefParser sp(filename);

  string net;
  double cap;
  bool valid = sp.read_net_cap(net, cap);
  int countWires = 0;
  Wire twire, twire2;
  set<Wire>::iterator itw;
  int i = 0;

  cout << " Number of wire nets: " << wires.size() << endl;
  while (valid) {
    // cout << "Lumped cap of net " << net << " is " << cap << endl ;
    ++i;
    twire.wire_name = net;

    itw = wires.find(twire);
    twire2 = *itw;
    // for (int i = 0; i < thisCircuit.wires.size(); ++i)
    //	if (net == thisCircuit.wires[i].wire_name)
    if (net == twire2.wire_name) {
      // thisCircuit.wires[i].cap = cap;
      twire.cap = cap;
      wires.erase(itw);
      wires.insert(twire);
      // it->cap = cap;
      ++countWires;
    }
    valid = sp.read_net_cap(net, cap);
  }
  cout << " Wire capacitances read: " << countWires << " (" << i << ")" << endl;
  // copy loads to cells
  set<Vcell *>::iterator it;
  Vcell *tmpCell;

  for (int i = 0; i < icells.size(); ++i) {
    tmpCell = icells[i];
    for (int k = 0; k < tmpCell->pinNetPairs.size(); ++k)
      if (tmpCell->pinNetPairs[k].first == "o") {
        twire.wire_name = tmpCell->pinNetPairs[k].second;
        twire.cap = 0.0;
        itw = wires.find(twire);
        twire2 = *itw;
        tmpCell->wireLoad = twire2.cap;
        // cout << tmpCell->wireLoad << endl;
        break;
      }
  }
  for (int i = 0; i < graphRoot.nextCells.size(); ++i) {
    tmpCell = graphRoot.nextCells[i];
    for (int k = 0; k < tmpCell->pinNetPairs.size(); ++k)
      if (tmpCell->pinNetPairs[k].first == "o") {
        twire.wire_name = tmpCell->pinNetPairs[k].second;
        twire.cap = 0.0;
        itw = wires.find(twire);
        twire2 = *itw;
        tmpCell->wireLoad = twire2.cap;
        // cout << tmpCell->wireLoad << endl;
        break;
      }
  }
}

void Circuit::printTree(Vcell *root) {
  // print circuit tree
  // shouldn't be used

  queue<Vcell *> fifo;
  Vcell *tmp;

  cout << "\nCircuit tree..." << endl;
  fifo.push(root);

  while (!fifo.empty()) {
    tmp = fifo.front();
    fifo.pop();
    if (tmp->actualInstType != NULL)
      cout << " Cell name: " << tmp->instName
           << " Cell instance: " << tmp->instType << " ("
           << tmp->actualInstType->name << ")" << endl;
    else
      cout << " Cell name: " << tmp->instName
           << " Cell instance: " << tmp->instType << endl;
    for (int i = 0; i < tmp->nextCells.size(); ++i)
      fifo.push(tmp->nextCells[i]);
  }
  cout << "Circuit tree end!\n" << endl;
}

void Circuit::readInputFiles() {
  // read all input files
  cout << "Reading library..." << endl;
  readLib();
  cout << "Reading library...done" << endl << endl;
  sortCells();

  cout << "Reading verilog..." << endl;
  readVerilog();
  cout << "Reading verilog...done" << endl << endl;

  cout << "Reading SDC..." << endl;
  readSDC();
  cout << "Reading SDC...done" << endl << endl;

  cout << "Reading SPEF..." << endl;
  readSPEF();
  cout << "Reading SPEF...done" << endl << endl;
}

// -----------------------------------------------------------------------------

void Circuit::printTiming(const string &label) {
  const double slack = getWorstSlack();

  const int x0 = timingTailNets.size() * 2;
  const double x1 =
      myRound(100 * (timingNumPathsWithNegativeSlack / double(x0)), 2);
  const double T = sdcInfos.clk_period;

  const double roundedLoadViol = myRound(loadViol, 2);
  const double roundedSlewViol = myRound(timingViolationSlew, 2);

  const double roundedTimingViol = myRound(timingTotalNegativeSlack, 2);
  const double roundedWorstSlack = myRound(getWorstSlack(), 2);

  cout << "\n";
  cout << "Timing Information (T=" << T << "): " << label << "\n";

  cout << "\tWorst Slack.....: " << roundedWorstSlack << " [" << slack
       << "]" /*<< " (" << minSlack << ")"*/ << "\n";
  // cout << "\tWorst Slew......: " << worstSlew << " (" << minSlew << ")" <<
  // "\n"; cout << "\tWorst Delay.....: " << worstDelay << "\n";
  cout << "\tLeakage.........: " << (totalLeakage / 1e6) << "\n";
  cout << "\tTiming Violation: " << roundedTimingViol << " ["
       << timingTotalNegativeSlack
       << "] #Paths: " << timingNumPathsWithNegativeSlack << "/" << (x0) << " ("
       << x1 << "%)"
       << "\n";
  cout << "\tSlew Violation..: " << roundedSlewViol << " ["
       << timingViolationSlew << "]"
       << "\n";
  cout << "\tLoad Violation..: " << roundedLoadViol << "\n";
  cout << endl;
}  // end method

// -----------------------------------------------------------------------------

void Circuit::printTimingDigest(const bool header, const string &title) {
  static int iteration = 0;

  const int N = 12;
  const int M = 80;

  const double slack = getWorstSlack();

  const int x0 = timingTailNets.size() * 2;
  const double x1 =
      myRound(100 * (timingNumPathsWithNegativeSlack / double(x0)), 2);
  const double T = sdcInfos.clk_period;

  const double roundedLoadViol = myRound(loadViol, 2);
  const double roundedSlewViol = myRound(timingViolationSlew, 2);

  const double roundedTimingViol = myRound(timingTotalNegativeSlack, 2);
  const double roundedWorstSlack = myRound(getWorstSlack(), 2);

  if (header) {
    iteration = 1;

    cout << setw(M) << setfill('=') << "\n" << setfill(' ');

    if (title != "") {
      cout << setw((M + title.length()) / 2) << title << "\n";
      cout << setw(M) << setfill('=') << "\n" << setfill(' ');
    }  // end if

    cout << setw(4) << "#" << setw(N) << "Worst" << setw(N) << "Leakage"
         << setw(N) << "Timing" << setw(N) << "Slew" << setw(N) << "Load"
         << setw(N) << "Path"
         << "\n";

    cout << setw(4) << "" << setw(N) << "Slack" << setw(N) << "(W)" << setw(N)
         << "Violation" << setw(N) << "Violation" << setw(N) << "Violation"
         << setw(N) << "Violation"
         << "\n";

    cout << setw(M) << setfill('=') << "\n" << setfill(' ');

    cout << setw(4) << "-";
  } else {
    cout << setw(4) << iteration++;
  }

  cout << setw(N) << roundedWorstSlack << setw(N) << (totalLeakage / 1e6)
       << setw(N) << roundedTimingViol << setw(N) << roundedSlewViol << setw(N)
       << roundedLoadViol << setw(N) << timingNumPathsWithNegativeSlack;

  cout << endl;
}  // end method

// -----------------------------------------------------------------------------

void Circuit::printPathTails() {
  const int pathSize = pathTails.size();

  for (int i = 0; i < pathSize; ++i)
    cout << pathTails[i]->instName << "\t" << pathTails[i]->actualRiseSlack
         << "\t" << pathTails[i]->actualFallSlack << endl;

}  // end method

// -----------------------------------------------------------------------------

void Circuit::printLibraryReport(ostream &out) {
  const int numOrgCells = orgCells.oCells.size();
  for (int i = 0; i < numOrgCells; i++) {
    OrdCells &ordCell = orgCells.oCells[i];

    cerr << "Cell: " << ordCell.footprint << "\n";

    const int numCellSizes = ordCell.cells.size();
    for (int k = 0; k < numCellSizes; k++) {
      LibParserCellInfo &cellinfo = ordCell.cells[k];

      const double capSum = computeAvgInputCapacitance(cellinfo);
      const double slew = maxTransition / 2;

      if (cellinfo.timingArcs.size() > 0) {
        const double outRiseDelay =
            Circuit::lookup(cellinfo.timingArcs[0].riseDelay, capSum, slew);
        const double outFallDelay =
            Circuit::lookup(cellinfo.timingArcs[0].fallDelay, capSum, slew);

        out << "\t" << k << "\t" << cellinfo.leakagePower << "\t"
            << cellinfo.area << "\t" << capSum << "\t" << outRiseDelay << "\t"
            << outFallDelay << "\n";
      }  // end if
    }    // end for
  }      // end for
}  // end method

// -----------------------------------------------------------------------------

void Circuit::StepperPrint::operator()(Vcell *cell) {
  cerr << cell->vectorIndex << "\t" << cell->actualInstType->name << "\n";
  cerr << "\tSequential: " << cell->actualInstType->isSequential << "\n";
  cerr << "\tInput Driver: " << (cell->instName == "inputDriver") << "\n";
  cerr << "\tLogical Depth: " << cell->logicalDepth << "\n";
  cerr << "\tWorst Rise Path Slack: " << cell->worstRisePathSlack << "\n";
  cerr << "\tWorst Fall Path Slack: " << cell->worstFallPathSlack << "\n";
}  // end method

// -----------------------------------------------------------------------------

void Circuit::stepperPrint(Vcell *cell) {
  cerr << cell->vectorIndex << "\t" << cell->actualInstType->name << "\n";
  cerr << "\tSequential: " << cell->actualInstType->isSequential << "\n";
  cerr << "\tInput Driver: " << (cell->instName == "inputDriver") << "\n";
  cerr << "\tLogical Depth: " << cell->logicalDepth << "\n";
  cerr << "\tWorst Rise Path Slack: " << cell->worstRisePathSlack << "\n";
  cerr << "\tWorst Fall Path Slack: " << cell->worstFallPathSlack << "\n";
}  // end method

// -----------------------------------------------------------------------------

void Circuit::StepperPathLogicalEffort::operator()(int arcIndex) {
  const TimingArc &arc = super->timingArcs[arcIndex];
  Vcell *cell = arc.cell;

  // Compute logical effort.
  double le;
  for (int j = 0; j < super->footprintLEPairs.size(); ++j) {
    if (cell->actualInstType->footprint == super->footprintLEPairs[j].first) {
      le = super->footprintLEPairs[j].second;
      break;
    }  // end if
  }    // end for
  pathLogicalEffort *= le;

  // Compute branching effort.
  double con;
  const LibParserCellInfo &cellInfo = *cell->actualInstType;
  const LibParserTimingInfo &timingInfo = cellInfo.timingArcs[arc.lut];
  for (int i = 0; cellInfo.pins.size(); i++) {
    const LibParserPinInfo &pinInfo = cellInfo.pins[i];

    if (pinInfo.name == timingInfo.fromPin) {
      con = pinInfo.capacitance;
      break;
    }  // end if
  }    // end for
  cell->branchingEffort = (super->getTimingNetState(arc.driver).load) / con;
  pathBranchingEffort *= (super->getTimingNetState(arc.driver).load) / con;
  pathFanoutEffort *=
      (cell->actualLoad / super->computeAvgInputCapacitance(cellInfo));

  //
  previousCell = currentCell;
  currentCell = cell;
  numPathCells++;

}  // end method

// -----------------------------------------------------------------------------

void Circuit::StepperAssignSizeLogicalEffort::operator()(int arcIndex) {
  const TimingArc &arc = super->timingArcs[arcIndex];
  Vcell *cell = arc.cell;

  double le;
  double Cin;
  const int numCandidateCells =
      super->orgCells.oCells[cell->footprintIndex].cells.size();
  vector<double> candidateFanoutFaninRatio(numCandidateCells);
  // const LibParserCellInfo &cellInfo = *cell->actualInstType;

  for (int j = 0; j < super->footprintLEPairs.size(); ++j) {
    if (cell->actualInstType->footprint == super->footprintLEPairs[j].first) {
      le = super->footprintLEPairs[j].second;
      break;
    }  // end if
  }    // end for
  Cin = (cell->actualLoad * le) / gainPerStage;
  // Cin = (cell->actualLoad * le * cell->branchingEffort) / gainPerStage;
  // cerr << "A celula " << cell->instName << " com Cin " << Cin << endl;
  // Cin = (cell->actualLoad * le * cell->branchingEffort) /
  // (le*(cell->actualLoad/super->computeAvgInputCapacitance(cellInfo))); Cin =
  // gainPerStage / (1 / numPathCells);

  for (int i = 0; i < numCandidateCells; i++)
    candidateFanoutFaninRatio[i] = super->computeAvgInputCapacitance(
        super->orgCells.oCells[cell->footprintIndex].cells[i]);
  // super->computeFanoutFaninRatioLE(super->orgCells.oCells[cell->footprintIndex].cells[i],
  // cell->actualLoad);

  // Find a best-fit for the fanout-of-4.
  int bestCell = -1;
  double bestFit = numeric_limits<double>::max();

  for (int i = 0; i < numCandidateCells; i++) {
    // Compute fanout-of-n mismatch of the candidate cell itself.
    const double mismatch = fabs(candidateFanoutFaninRatio[i] - Cin);

    // Check if this is the lower mismatch
    if (fabs(mismatch) < fabs(bestFit)) {
      bestFit = mismatch;
      bestCell = i;
    }  // end if
  }    // end for

  // Change to the best cell candidate.
  super->updateCellType(cell, bestCell);

  // cerr << "Cell: " << cell->instName << " ("  <<cell->instType << ")\t" <<
  // Cin << "\t" << bestCell << "\n";
}  // end method

// -----------------------------------------------------------------------------

void Circuit::StepperAssignSizeLogicalEffortFanout::operator()(int arcIndex) {
  const double FANOUT = fanoutEffortPerStage;  // fanoutPerStage

  const TimingArc &arc = super->timingArcs[arcIndex];
  Vcell *cell = arc.cell;

  if (cell->dontTouch) return;

  const int numCandidateCells =
      super->orgCells.oCells[cell->footprintIndex].cells.size();

  // Compute the average input capacitance for candidate cells (same footprint
  // cells).
  vector<double> candidateFanoutFaninRatio(numCandidateCells);
  for (int i = 0; i < numCandidateCells; i++)
    candidateFanoutFaninRatio[i] = super->computeFanoutFaninRatioLE(
        super->orgCells.oCells[cell->footprintIndex].cells[i],
        cell->actualLoad);

  // Find a best-fit for the fanout-of-4.
  // const int originalTypeIndex = cell->actualInstTypeIndex;

  int bestCell = -1;
  double bestFit = numeric_limits<double>::max();

  for (int i = 0; i < numCandidateCells; i++) {
    // Compute fanout-of-n mismatch of the candidate cell itself.
    const double mismatch = fabs(candidateFanoutFaninRatio[i] - FANOUT);

    // Check if this is the lower mismatch
    if (fabs(mismatch) < fabs(bestFit)) {
      bestFit = mismatch;
      bestCell = i;
    }  // end if
  }    // end for

  // cerr << originalTypeIndex << " -> " << bestCell << "\t" << bestFit << "\n";

  // Change to the best cell candidate.
  super->updateCellType(cell, bestCell);
}  // end method

// -----------------------------------------------------------------------------

void Circuit::stepperCriticalPathUpdater(Vcell *cell) {
  criticalPath.push_back(cell);
}  // end method

// -----------------------------------------------------------------------------

void Circuit::stepperFo4(Vcell *cell) {
  const double FANOUT = 1.6;

  if (cell->dontTouch) return;

  const int numCandidateCells =
      orgCells.oCells[cell->footprintIndex].cells.size();

  // Compute the average input capacitance for candidate cells (same footprint
  // cells).
  vector<double> candidateFanoutFaninRatio(numCandidateCells);
  for (int i = 0; i < numCandidateCells; i++)
    candidateFanoutFaninRatio[i] = computeFanoutFaninRatio(
        orgCells.oCells[cell->footprintIndex].cells[i], cell->actualLoad);

  // Find a best-fit for the fanout-of-4.
  int bestCell = -1;
  double bestFit = numeric_limits<double>::max();

  for (int i = 0; i < numCandidateCells; i++) {
    // Compute fanout-of-n mismatch of the candidate cell itself.
    const double mismatch = fabs(candidateFanoutFaninRatio[i] - FANOUT);

    // Check if this is the lower mismatch
    if (fabs(mismatch) < fabs(bestFit)) {
      bestFit = mismatch;
      bestCell = i;
    }  // end if
  }    // end for

  // Change to the best cell candidate.
  updateCellType(cell, bestCell);
}  // end method

// -----------------------------------------------------------------------------

void Circuit::stepperFoX(Vcell *cell) {
  // const double FANOUT = 1.6;

  double FANOUT = 5.0;
  const double cellSlack =
      min(cell->worstRisePathSlack, cell->worstFallPathSlack);

  if (cellSlack < 0.0)
    FANOUT =
        3.5 / (0.1 + log10((pow(fabs(cellSlack), 1.35) + fabs(worstSlack)) /
                           fabs(worstSlack)));
  else
    return;

  assert(FANOUT > 0.0);

  if (cell->dontTouch) return;

  const int numCandidateCells =
      orgCells.oCells[cell->footprintIndex].cells.size();

  // Compute the average input capacitance for candidate cells (same footprint
  // cells).
  vector<double> candidateFanoutFaninRatio(numCandidateCells);
  for (int i = 0; i < numCandidateCells; i++)
    candidateFanoutFaninRatio[i] = computeFanoutFaninRatio(
        orgCells.oCells[cell->footprintIndex].cells[i], cell->actualLoad);

  // Find a best-fit for the fanout-of-4.
  int bestCell = -1;
  double bestFit = numeric_limits<double>::max();

  for (int i = 0; i < numCandidateCells; i++) {
    // Compute fanout-of-n mismatch of the candidate cell itself.
    const double mismatch = fabs(candidateFanoutFaninRatio[i] - FANOUT);

    // Check if this is the lower mismatch
    if (fabs(mismatch) < fabs(bestFit)) {
      bestFit = mismatch;
      bestCell = i;
    }  // end if
  }    // end for

  // Change to the best cell candidate.
  updateCellType(cell, bestCell);
}  // end method

// -----------------------------------------------------------------------------

void Circuit::sizingLogicalEffort() {
  updateTiming();
  assignLogicalEffort();
  updateTopCriticalTailNets();
  double cLoad, Cin, pathEffort, pathFanout;

  const int NUM_PATHS = arrivalTimingNet.size() / 4;
  // const int NUM_PATHS = 1;

  int counter = 0;

  // cerr << "Worst Arrival Time: "  << timingWorstArrivalTime << "\n";

  for (multimap<double, pair<int, EdgeType>>::reverse_iterator it =
           arrivalTimingNet.rbegin();
       it != arrivalTimingNet.rend(); it++) {
    // cout << "Arrival time: " << it->first << " net " << it->second << endl;
    if (sdcInfos.clk_period - it->first >= 0) break;

    const int n = it->second.first;
    const EdgeType edgeType = it->second.second;
    const TimingNet &net = timingNets[n];

    // cerr << "Sizing: " << n << "\t" << it->first << "\t"  << edgeType <<
    // "\n";

    StepperPathLogicalEffort stepper(this);
    walkFollowingBacktrackPointers(n, edgeType, stepper);

    if (!stepper.currentCell) continue;

    cLoad = net.driver->actualLoad;

    const string outputNet = timingNetName[stepper.currentCell->sinkNetIndex];

    const vector<pair<string, string>> &p = stepper.previousCell->pinNetPairs;
    const vector<LibParserPinInfo> &pins =
        stepper.previousCell->actualInstType->pins;

    //*
    Cin = 0;
    for (int i = 0; i < p.size(); i++) {
      if (p[i].second == outputNet) {
        for (int k = 0; k < p.size(); k++) {
          if (p[i].first == pins[k].name) {
            Cin = pins[k].capacitance;
            break;
          }  // end if
        }    // end for
      }      // end if
    }        // end for
    assert(Cin > 0);
    //*/
    // Cin = stepper.currentCell->actualLoad;

    pathFanout = cLoad / Cin;
    pathEffort =
        pathFanout * stepper.pathLogicalEffort * stepper.pathBranchingEffort;
    stepper.numPathCells = stepper.numPathCells - 1;
    // cout << "Numero de celulas do caminho " << stepper.numPathCells << endl;
    StepperAssignSizeLogicalEffort stepper1(this);
    stepper1.gainPerStage = pow(pathEffort, 1.0 / stepper.numPathCells);
    stepper1.numPathCells = stepper.numPathCells;
    StepperAssignSizeLogicalEffortFanout stepper2(this);
    stepper2.fanoutPerStage = pow(
        pathFanout,
        1.0 / stepper
                  .numPathCells);  // can multiply by 2 only for a test, because
                                   // fanoutPerStage was a very small value
    stepper2.fanoutEffortPerStage =
        pow(stepper.pathFanoutEffort, 1.0 / stepper.numPathCells);

    // cerr << "net " << it->second.first << " fanout por estÃ¡gio " <<
    // stepper2.fanoutPerStage << endl;

    // walkFollowingBacktrackPointers(n, edgeType, stepper1 );
    walkFollowingBacktrackPointers(n, edgeType, stepper1);

    if (++counter >= NUM_PATHS) break;

  }  // end for

}  // end method

// -----------------------------------------------------------------------------

void Circuit::stepperFoXLogicalEffort(Vcell *cell) {
  if (cell->dontTouch) return;

  double FANOUT = 3.6;

  const TimingNetState &netstate = getTimingNetState(cell->sinkNetIndex);

  int initialCell = max(0, cell->actualInstTypeIndex - 1);
  const double riseSlack = -(netstate.arrivalTime[RISE] -
                             timingRequiredTime[cell->sinkNetIndex][RISE]);
  const double fallSlack = -(netstate.arrivalTime[FALL] -
                             timingRequiredTime[cell->sinkNetIndex][FALL]);
  const double cellSlack = min(riseSlack, fallSlack);

  // cout << cell->instName << "\t" << cellSlack << endl;

  if (cellSlack < 0.0) {
    FANOUT = 3.6;  ///((fabs(cellSlack)/fabs(sdcInfos.clk_period) ));
    const int diff = 29 - cell->actualInstTypeIndex;
    initialCell = int(cell->actualInstTypeIndex +
                      (min(3, diff)) * (fabs(cellSlack) / fabs(worstSlack)));
  }
  // else return;
  assert(initialCell < 30);

  const int numCandidateCells =
      orgCells.oCells[cell->footprintIndex].cells.size();

  // Compute the average input capacitance for candidate cells (same footprint
  // cells).
  vector<double> candidateFanoutFaninRatio(numCandidateCells);
  for (int i = 0; i < numCandidateCells; i++)
    candidateFanoutFaninRatio[i] = computeFanoutFaninRatioLE(
        orgCells.oCells[cell->footprintIndex].cells[i], cell->actualLoad);

  // Find a best-fit for the fanout-of-4.
  int bestCell = -1;
  double bestFit = numeric_limits<double>::max();

  for (int i = initialCell; i < numCandidateCells; i++) {
    // Compute fanout-of-n mismatch of the candidate cell itself.
    const double mismatch = fabs(candidateFanoutFaninRatio[i] - FANOUT);

    // Check if this is the lower mismatch
    if (fabs(mismatch) < fabs(bestFit)) {
      bestFit = mismatch;
      bestCell = i;
    }  // end if
  }    // end for

  // Change to the best cell candidate.
  updateCellType(cell, bestCell);
}  // end method

// -----------------------------------------------------------------------------

void Circuit::stepperSlewTarget(Vcell *cell) {
  int initialCell = cell->actualInstTypeIndex;
  const double cellSlack =
      min(cell->worstRisePathSlack, cell->worstFallPathSlack);

  if (cell->dontTouch) return;

  // atualizar os valores de Slew para cada possível célula considerando o slew
  // target da anterior

  //	for (int i = 0; i<icells.size(); ++i)
  //	{
  //		tmpCell = icells[i];
  //
  //		if (tmpCell->actualInstType->isSequential)
  ////&& ((tmpCell->pinNetPairs[j].first == "ck") ||
  //(tmpCell->pinNetPairs[j].first == "d")) ) )
  // tmpCell->pinOk = tmpCell->previousCells.size();
  ////FF inputs "ck" and "d" are always OK 		for (int j = 0;
  // j<tmpCell->pinNetPairs.size(); ++j)
  //		{
  //			tmpCell->arrivalTimes[j].first =
  // tmpCell->arrivalTimes[j].second = 0.0;
  // tmpCell->inputSlews[j].first = tmpCell->inputSlews[j].second = 0.0;
  //		}
  //		if (tmpCell->actualInstType->isSequential)
  //		{
  //			qCircuit.push(tmpCell);
  //		}
  //	}
  //
  //	worstSlew =
  // max(worstSlew,max(tmpCell->outputRiseSlew,tmpCell->outputFallSlew));

  //	for (int j = 0; j < tmpCell->nextCells.size(); ++j)
  //		{
  //			//cout << "next cell name: " <<
  // tmpCell->nextCells[j]->instName
  //<< endl; 			if
  //(!tmpCell->nextCells[j]->actualInstType->isSequential)
  //			{
  //				++tmpCell->nextCells[j]->pinOk;
  //
  //				if ( tmpCell->nextCells[j]->pinOk ==
  // tmpCell->nextCells[j]->previousCells.size() )
  //				{
  //					qCircuit.push(tmpCell->nextCells[j]);
  //				}
  //			}
  //			for (int k = 0; k <
  // tmpCell->nextCells[j]->pinNetPairs.size();
  //++k) 				if
  //(tmpCell->nextCells[j]->pinNetPairs[k].second
  //== tmpCell->pinNetPairs.back().second)
  //				{
  //					assert(tmpCell->pinNetPairs.back().first
  //== "o");
  //					//if (tmpCell->pinNetPairs.back().first
  //!= "o") cerr
  //<< "ERRO NO TIMER!!!\nSAIDA DA PORTA NAO EH O ULTIMO PINO!" << endl;
  //
  //					tmpCell->nextCells[j]->inputSlews[k].first
  //= tmpCell->outputRiseSlew;
  // tmpCell->nextCells[j]->inputSlews[k].second = tmpCell->outputFallSlew;
  // tmpCell->nextCells[j]->arrivalTimes[k].first = worstOutputRiseArrival;
  // tmpCell->nextCells[j]->arrivalTimes[k].second = worstOutputFallArrival;
  //
  //					if
  //(tmpCell->nextCells[j]->actualInstType->isSequential)
  //					{
  //						tmpCell->nextCells[j]->arrivalTimes[k].first
  //=
  // 0.0;
  // tmpCell->nextCells[j]->arrivalTimes[k].second = 0.0;
  //					}
  //
  //				}
  //		}
  //
  //	double Circuit::lookupDelay( const LibParserTimingInfo &timingInfo,
  // const EdgeType edgeType, const double inputSlew, const double
  // loadCapacitance ) { 	const LibParserLUT &lut = edgeType == FALL?
  // timingInfo.fallDelay : timingInfo.riseDelay; 	const double x =
  // loadCapacitance; 	const double y = inputSlew;
  //
  //	return lookup( lut, x, y );
  //
  //	lookupOutputSlew
  //	computeArcTiming
  //	// Output Slew
  //	outSlew[RISE] = Circuit::lookup(timingInfo.riseTransition, xOutputLoad,
  // yInputFallSlew ); 	outSlew[FALL] =
  // Circuit::lookup(timingInfo.fallTransition, xOutputLoad, yInputRiseSlew );
  //
  //	cell->inputSlews[j].second : timingInfo.riseDelay.transitionIndices[0];

  int i, k;
  // cout << "O slew da célula " << cell->instType << " é " <<
  // cell->outputRiseSlew << " fall " << cell->outputFallSlew << endl;

  for (i = 0; i < cell->actualInstType->timingArcs.size(); ++i) {
    for (k = 0; k < cell->pinNetPairs.size(); ++k) {
      if (cell->actualInstType->timingArcs[i].fromPin ==
          cell->pinNetPairs[k].first)
        break;
    }
    assert(k != cell->pinNetPairs.size());

    if (cell->actualInstType->timingArcs[i].toPin != "o") continue;

    cout << "celula " << cell->instName << " entrada "
         << cell->pinNetPairs[k].first << " input slew do pino " << k << " é "
         << cell->inputSlews[k].second << endl;

    // net.slew[RISE] = cell->inputSlews[0].first;
    // net.slew[FALL] = cell->inputSlews[0].second;

  }  // end for

  //	const int numCandidateCells =
  // orgCells.oCells[cell->footprintIndex].cells.size();
  //
  //	// Compute the average input capacitance for candidate cells (same
  // footprint cells). 	vector<double> candidateFanoutFaninRatio(
  // numCandidateCells ); 	for ( int i = 0; i < numCandidateCells; i++ )
  //		candidateFanoutFaninRatio[i] =
  // computeFanoutFaninRatioLE(orgCells.oCells[cell->footprintIndex].cells[i],
  // cell->actualLoad);
  //
  //	// Find a best-fit for the fanout-of-4.
  //	int bestCell = -1;
  //	double bestFit = numeric_limits<double>::max();
  //
  //	for ( int i = initialCell; i < numCandidateCells; i++ ) {
  //		// Compute fanout-of-n mismatch of the candidate cell itself.
  //		const double mismatch = fabs( candidateFanoutFaninRatio[i] -
  // FANOUT
  //);
  //
  //		// Check if this is the lower mismatch
  //		if ( fabs(mismatch) < fabs(bestFit) ) {
  //			bestFit = mismatch;
  //			bestCell = i;
  //		} // end if
  //	} // end for
  //
  //	// Change to the best cell candidate.
  //	updateCellType(cell, bestCell);
}  // end method

// -----------------------------------------------------------------------------

void Circuit::stepperFoXLogicalEffortPosContest(Vcell *cell) {
  double FANOUT = 2.0;

  int initialCell = cell->actualInstTypeIndex;
  const double cellSlack =
      min(cell->worstRisePathSlack, cell->worstFallPathSlack);

  if (cellSlack < 0.0) {
    FANOUT = 2.0;  ///((fabs(cellSlack)/fabs(sdcInfos.clk_period) ));
    initialCell = 21 * (fabs(cellSlack) / fabs(worstSlack));
  } else
    return;

  assignLogicalEffortContestLibrary();

  // const double FANOUT = 2.5;

  if (cell->dontTouch) return;

  const int numCandidateCells =
      orgCells.oCells[cell->footprintIndex].cells.size();

  // Compute the average input capacitance for candidate cells (same footprint
  // cells).
  vector<double> candidateFanoutFaninRatio(numCandidateCells);
  for (int i = 0; i < numCandidateCells; i++)
    candidateFanoutFaninRatio[i] = computeFanoutFaninRatioLEPosContest(
        orgCells.oCells[cell->footprintIndex].cells[i], cell->actualLoad);

  // Find a best-fit for the fanout-of-4.
  int bestCell = -1;
  double bestFit = numeric_limits<double>::max();

  for (int i = 0; i < numCandidateCells; i++) {
    // Compute fanout-of-n mismatch of the candidate cell itself.
    const double mismatch = fabs(candidateFanoutFaninRatio[i] - FANOUT);

    // Check if this is the lower mismatch
    if (fabs(mismatch) < fabs(bestFit)) {
      bestFit = mismatch;
      bestCell = i;
    }  // end if
  }    // end for

  // Change to the best cell candidate.
  updateCellType(cell, bestCell);
}  // end method

// -----------------------------------------------------------------------------

void Circuit::stepperFoXLogicalEffortCopy(Vcell *cell) {
  const double FANOUT = 32;  // original 1.6

  if (cell->dontTouch) return;

  const int numCandidateCells =
      orgCells.oCells[cell->footprintIndex].cells.size();

  // Compute the average input capacitance for candidate cells (same footprint
  // cells).
  vector<double> candidateFanoutFaninRatio(numCandidateCells);
  for (int i = 0; i < numCandidateCells; i++)
    candidateFanoutFaninRatio[i] = computeFanoutFaninRatioLE(
        orgCells.oCells[cell->footprintIndex].cells[i], cell->actualLoad);

  // Find a best-fit for the fanout-of-4.
  int bestCell = -1;
  double bestFit = numeric_limits<double>::max();

  for (int i = 0; i < numCandidateCells; i++) {
    // Compute fanout-of-n mismatch of the candidate cell itself.
    const double mismatch = fabs(candidateFanoutFaninRatio[i] - FANOUT);

    // Check if this is the lower mismatch
    if (fabs(mismatch) < fabs(bestFit)) {
      bestFit = mismatch;
      bestCell = i;
    }  // end if
  }    // end for

  // Change to the best cell candidate.
  updateCellType(cell, bestCell);
}  // end method

// -----------------------------------------------------------------------------

void Circuit::stepperNoLoadViolation(Vcell *cell) {
  // Find the cell with less input capacitance able to drive its load.
  int bestCell = -1;
  double bestAvgInputCapacitance = +numeric_limits<double>::max();

  const int numCandidateCells =
      orgCells.oCells[cell->footprintIndex].cells.size();
  for (int i = 0; i < numCandidateCells; i++) {
    const LibParserCellInfo &cellinfo =
        orgCells.oCells[cell->footprintIndex].cells[i];

    double avgInputCapacitance = 0;
    double maxOutputLoad = 0;

    const int numPins = cellinfo.pins.size();
    for (int k = 0; k < numPins; k++) {
      const LibParserPinInfo &pininfo = cellinfo.pins[k];

      if (pininfo.isInput)
        avgInputCapacitance += pininfo.capacitance;
      else
        maxOutputLoad = max(maxOutputLoad, pininfo.maxCapacitance);
    }  // end for
    avgInputCapacitance /= (numPins - 1);

    if (maxOutputLoad >= cell->actualLoad) {
      if (avgInputCapacitance < bestAvgInputCapacitance) {
        bestCell = i;
        bestAvgInputCapacitance = avgInputCapacitance;
      }  // end if
    }    // end if
  }      // end for

  if (bestCell != -1)
    if (!cell->dontTouch) updateCellType(cell, bestCell);
}  // end method

// -----------------------------------------------------------------------------

void Circuit::stepperForNoLoadAndSlewViolationByLiLi(Vcell *cell) {
  if (cell->dontTouch) return;

  // Find the cell with less leakage power respecting slew and load
  // constraints. Assuming cells are sorted by less leakage power.

  const double alphaLoad = 0.7;

  int bestLoad = -1;
  int bestSlew = -1;

  // Sweep cells from lowest leakage to the largest leakage.
  const int numCandidateCells =
      orgCells.oCells[cell->footprintIndex]
          .cells.size();  // sorted cells by leakage power
  for (int i = 0; i < numCandidateCells; i++) {
    const LibParserCellInfo &cellinfo =
        orgCells.oCells[cell->footprintIndex].cells[i];

    // [TODO] Consider storing the max output load for a cell type to avoid
    // sweeping all pins  over and over again to find out the max output
    // capacitance :)

    double maxOutputLoad = 0;
    const int numPins = cellinfo.pins.size();
    for (int k = 0; k < numPins; k++) {
      const LibParserPinInfo &pininfo = cellinfo.pins[k];

      if (!pininfo.isInput)
        maxOutputLoad = max(maxOutputLoad, pininfo.maxCapacitance);
    }  // end for

    const bool meetLoadConstraint =
        (alphaLoad * maxOutputLoad > cell->actualLoad);

    const bool meetSlewConstraint = (cell->outputFallSlew < maxTransition) &&
                                    (cell->outputRiseSlew < maxTransition);

    if (meetLoadConstraint && bestLoad == -1) bestLoad = i;
    if (meetSlewConstraint && bestSlew == -1) bestSlew = i;

    if (bestLoad != -1 && bestSlew != -1) break;
  }  // end for

  if (bestLoad != -1 || bestSlew != -1) {
    updateCellType(cell, max(bestLoad, bestSlew));
  }  // end for
}  // end method

// -----------------------------------------------------------------------------

void Circuit::sizingForNoLoadAndSlewViolationByLiLi() {
  walkBackwardDepth(&Circuit::stepperForNoLoadAndSlewViolationByLiLi);
}  // end method

// -----------------------------------------------------------------------------

void Circuit::stepperForNoLoadAndSlewViolationByLivramento(Vcell *cell) {
  if (cell->dontTouch) return;

  // Find the cell with less leakage power respecting slew and load
  // constraints. Assuming cells are sorted by less leakage power.

  int bestLoad = -1;

  // Sweep cells from lowest leakage to the largest leakage.
  const int numCandidateCells =
      orgCells.oCells[cell->footprintIndex]
          .cells.size();  // sorted cells by leakage power
  for (int i = 0; i < numCandidateCells; i++) {
    const LibParserCellInfo &cellinfo =
        orgCells.oCells[cell->footprintIndex].cells[i];

    // [TODO] Consider storing the max output load for a cell type to avoid
    // sweeping all pins  over and over again to find out the max output
    // capacitance :)

    double maxOutputLoad = 0;
    const int numPins = cellinfo.pins.size();
    for (int k = 0; k < numPins; k++) {
      const LibParserPinInfo &pininfo = cellinfo.pins[k];

      if (!pininfo.isInput)
        maxOutputLoad = max(maxOutputLoad, pininfo.maxCapacitance);
    }  // end for

    if (maxOutputLoad >= cell->actualLoad) {
      bestLoad = i;
      break;
    }
  }  // end for

  if (bestLoad != -1) {
    updateCellType(cell, bestLoad);
  }  // end for
}  // end method

// -----------------------------------------------------------------------------

void Circuit::sizingForNoLoadAndSlewViolationByLivramento() {
  const int numCells = depthSortedCells.size();
  for (int i = offsetCombinational; i < numCells; i++)
    updateCellType(depthSortedCells[i], 0);

  walkBackwardDepth(&Circuit::stepperForNoLoadAndSlewViolationByLivramento);
}  // end method

// -----------------------------------------------------------------------------

void Circuit::changeCellsFlach1() {
  stepperFo4(chooseCellRandomlyFromVector(icells));
}  // end method

// -----------------------------------------------------------------------------

void Circuit::changeCellsFlach2() {
  const double FANOUT = 4;

  // change cells
  Vcell *chosenCell = NULL;

  // Randomly choose a cell.
  chosenCell = chooseCellRandomlyFromVector(icells);

  const int numPreviousCells = chosenCell->previousCells.size();
  const int numCandidateCells =
      orgCells.oCells[chosenCell->footprintIndex].cells.size();

  // Compute the average input capacitance for previous cells.
  vector<double> AvgInputCapOfPreviousCells(numPreviousCells);
  for (int i = 0; i < numPreviousCells; i++) {
    Vcell *cell = chosenCell->previousCells[i];

    const LibParserCellInfo &cellinfo =
        orgCells.oCells[cell->footprintIndex].cells[cell->actualInstTypeIndex];

    AvgInputCapOfPreviousCells[i] = 0;

    const int numPins = cellinfo.pins.size();
    for (int k = 0; k < numPins; k++) {
      const LibParserPinInfo &pininfo = cellinfo.pins[k];

      AvgInputCapOfPreviousCells[i] += pininfo.capacitance;
    }  // end for

    AvgInputCapOfPreviousCells[i] /= numPins;
  }  // end for

  // Compute the average input capacitance for candidate cells (same footprint
  // cells).
  vector<double> AvgInputCapOfCandidateCells(numCandidateCells);
  for (int i = 0; i < numCandidateCells; i++) {
    const LibParserCellInfo &cellinfo =
        orgCells.oCells[chosenCell->footprintIndex].cells[i];

    AvgInputCapOfCandidateCells[i] = 0;

    const int numPins = cellinfo.pins.size();
    for (int k = 0; k < numPins; k++) {
      const LibParserPinInfo &pininfo = cellinfo.pins[k];

      AvgInputCapOfCandidateCells[i] += pininfo.capacitance;
    }  // end for

    AvgInputCapOfCandidateCells[i] /= numPins;
  }  // end for

  // Find a best-fit for the fanout-of-4.
  int bestCell = -1;
  double bestFit = numeric_limits<double>::max();

  for (int i = 0; i < numCandidateCells; i++) {
    // Temporarily changes to the candidate cell.
    updateCellType(chosenCell, i);

    double mismatch = 0;

    // Compute fanout-of-n mismatch of previous cells.
    for (int k = 0; k < numPreviousCells; k++) {
      Vcell *cell = chosenCell->previousCells[k];

      updateCellLoad(cell);

      mismatch +=
          fabs(cell->actualLoad / AvgInputCapOfPreviousCells[k] - FANOUT);
    }  // end for

    // Compute fanout-of-n mismatch of the candidate cell itself.
    mismatch +=
        fabs(chosenCell->actualLoad / AvgInputCapOfCandidateCells[i] - FANOUT);

    // Check if this is the lower mismatch

    if (fabs(mismatch) < fabs(bestFit)) {
      bestFit = mismatch;
      bestCell = i;
    }  // end if
  }    // end for

  // cout << "******** cell: " << chosenCell->instName << " changed from: " <<
  // chosenCell->instType;

  // Change to the best cell candidate.
  updateCellType(chosenCell, bestCell);

  // Update capacitive load for previos cells.
  for (int k = 0; k < numPreviousCells; k++)
    updateCellLoad(chosenCell->previousCells[k]);

  // cout << " to: " << chosenCell->instType << endl;

}  // end method

// -----------------------------------------------------------------------------

void Circuit::changeCellsFlach3() {
  /*	Vcell * chosenCell = NULL;

          // Randomly choose a cell.
          chosenCell = chooseCellRandomlyFromVector(criticalPath);

          const int numCandidateCells =
     orgCells.oCells[chosenCell->footprintIndex].cells.size();

          const int originalTypeIndex = chosenCell->actualInstTypeIndex;
          const int randomTypeIndex = floor((rand() /
     double(RAND_MAX))*(numCandidateCells - 1) + 0.5);

          const double originalSlack = worstSlack;

          updateCellType(chosenCell, randomTypeIndex);
          coneTiming2(chosenCell);

          if (worstSlack < originalSlack) {
                  // Undo.
                  updateCellType(chosenCell, originalTypeIndex);

                  coneTiming2(chosenCell);
          } // end if

          /*
           bool changed = false;
           if ( originalSlack < 0 )
           changed = updateCellTypeToNextFaster( chosenCell );
           else if ( originalSlack > 0 )
           changed = updateCellTypeToNextSlower( chosenCell );

           if ( changed ) {
           coneTiming(chosenCell);
           calcTimingViol();

           if ( worstSlack < originalSlack ) {
           // Undo.
           updateCellType( chosenCell, originalTypeIndex );
           coneTiming(chosenCell);
           calcTimingViol();
           } // end if

           } // end if
           */

}  // end method

// -----------------------------------------------------------------------------

void Circuit::changeCellsGraci() {
  // change cells
  Vcell *tmpCell = NULL;

  // Print output gates
  vector<pair<string, Vcell *>> outputs;
  double initialWorstSlack = worstSlack;

  for (int i = 0; i < graphRoot.nextCells.size(); ++i) {
    graphRoot.nextCells[i]->changed = false;
    cout << "Nome do graphroot " << graphRoot.instName << " e tipo "
         << graphRoot.instType << endl;
    cout << "Proxima celula do graphroot " << graphRoot.nextCells[i]->instName
         << " e tipo " << graphRoot.nextCells[i]->instType << endl;
  }

  cout << "O slack Ã© " << initialWorstSlack << endl;

  for (int i = 0; i < outputs.size(); ++i) {
    tmpCell = outputs[i].second;

    while (tmpCell != &graphRoot) {
      // cout << "Na saida " << outputs[i].first << " tem-se a celula " <<
      // outputs[i].second->instName << endl;

      const double CLoad = tmpCell->actualLoad;
      cout << "Cload da celula = " << CLoad << endl;

      const int numCells =
          orgCells.oCells[tmpCell->footprintIndex].cells.size();
      cout << "numCells = " << numCells << endl;

      int j = 0;
      while (j < numCells && worstSlack <= initialWorstSlack) {
        cout << "Worst slack atual " << worstSlack
             << " e o worst slack anterior " << initialWorstSlack << endl
             << endl;

        const LibParserCellInfo &cellinfo =
            orgCells.oCells[tmpCell->footprintIndex].cells[j];

        double maxPinCap = 0;

        const int numPins = cellinfo.pins.size();
        cout << "O numero de pinos Ã© " << numPins << endl << endl;
        for (int k = 0; k < numPins; k++) {
          const LibParserPinInfo &pininfo = cellinfo.pins[k];
          if (!pininfo.isInput) maxPinCap = pininfo.maxCapacitance;
          cout << "Maxima capacitancia = " << maxPinCap << endl;
          // setar essa instÃ¢ncia para ser minha cÃ©lula, calcular o timing,
          // analisar se o slack aumentou, desfaz e tenta com a seguinte cÃ©lula
        }  // end for
        if (maxPinCap >= CLoad) {
          updateCellType(outputs[i].second, j);
        }
        // calcTiming();
        updateTiming();
        j++;
      }  // end while

      cout << "Numero de celulas anteriores "
           << outputs[i].second->previousCells.size() << endl;

      // tmpCell = tmpCell->previousCells;

      for (int l = 0; l < outputs[i].second->previousCells.size(); ++l) {
        tmpCell = outputs[i].second->previousCells[l];
        cout << "Celula anterior " << l << " eh um "
             << outputs[i].second->previousCells[l]->instName << endl;
        for (int m = 0; m < tmpCell->previousCells.size(); ++m)
          cout << "Celula anterior " << m << " eh um "
               << tmpCell->previousCells[m]->instName << endl;
      }

    }  // end while

  }  // end for

}  // end method

// -----------------------------------------------------------------------------

void Circuit::walkBackwardFromTemporalBarriers(WalkerCallback callback) {
  const int numNodes = icells.size();
  const int numTailNodes = pathTails.size();

  stopWalking = false;

  // Queue.
  queue<Vcell *> q;

  // Clear visited node flag.
  for (int i = 0; i < numNodes; i++) icells[i]->visitcount = 0;

  // Put primary circuit outputs into the queue.
  for (int i = 0; i < numTailNodes; i++) q.push(pathTails[i]);

  // Walk
  while (!q.empty()) {
    Vcell *cell = q.front();
    q.pop();

    if (cell->visitcount) continue;

    cell->visitcount = 1;
    (this->*callback)(cell);

    if (stopWalking) return;

    const int numPreviousCells = cell->previousCells.size();
    for (int i = 0; i < numPreviousCells; i++) {
      Vcell *driver = cell->previousCells[i];

      if (!driver->visitcount) q.push(driver);
    }  // end for
  }    // end while
}  // end method

// -----------------------------------------------------------------------------

void Circuit::walkForwardFromTemporalBarriers(WalkerCallback callback) {
  const int numNodes = icells.size();
  const int numHeadNodes = pathHeads.size();

  stopWalking = false;

  // Queue.
  queue<Vcell *> q;

  // Clear visited node flag.
  for (int i = 0; i < numNodes; i++) icells[i]->visitcount = 0;

  // Put primary circuit outputs into the queue.
  for (int i = 0; i < numHeadNodes; i++) q.push(pathHeads[i]);

  // Walk
  while (!q.empty()) {
    Vcell *cell = q.front();
    q.pop();

    if (cell->visitcount) continue;

    cell->visitcount = 1;
    (this->*callback)(cell);

    if (stopWalking) return;

    const int numNextCells = cell->nextCells.size();
    for (int i = 0; i < numNextCells; i++) {
      Vcell *driver = cell->nextCells[i];

      if (!driver->visitcount) q.push(driver);
    }  // end for
  }    // end while
}  // end method

// -----------------------------------------------------------------------------

void Circuit::walkBackward(WalkerCallback callback) {
  const int numNodes = icells.size();
  const int numPrimaryOutputs = outputs.size();

  // Queue.
  queue<Vcell *> q;

  // Clear visited node flag.
  for (int i = 0; i < numNodes; i++) icells[i]->visitcount = 0;

  // Put primary circuit outputs into the queue.
  for (int i = 0; i < numPrimaryOutputs; i++) q.push(outputs[i].second);

  // Walk
  while (!q.empty()) {
    Vcell *cell = q.front();
    q.pop();

    if (cell->visitcount) continue;

    cell->visitcount = 1;
    (this->*callback)(cell);

    const int numPreviousCells = cell->previousCells.size();
    for (int i = 0; i < numPreviousCells; i++) {
      Vcell *driver = cell->previousCells[i];

      if (!driver->visitcount) q.push(driver);
    }  // end for
  }    // end while
}  // end method

// -----------------------------------------------------------------------------

void Circuit::walkForward(WalkerCallback callback) {
  const int numNodes = icells.size();

  // Queue.
  queue<Vcell *> q;

  // Clear visited node flag.
  for (int i = 0; i < numNodes; i++) icells[i]->visitcount = 0;

  // Put primary circuit outputs into the queue.
  for (int i = 0; i < graphRoot.nextCells.size(); i++)
    for (int k = 0; k < graphRoot.nextCells[i]->nextCells.size(); k++)
      q.push(graphRoot.nextCells[i]->nextCells[k]);

  // Walk
  while (!q.empty()) {
    Vcell *cell = q.front();
    q.pop();

    if (cell->visitcount) continue;

    cell->visitcount = 1;
    (this->*callback)(cell);

    const int numNextCells = cell->nextCells.size();
    for (int i = 0; i < numNextCells; i++) {
      Vcell *driver = cell->nextCells[i];

      if (!driver->visitcount) q.push(driver);
    }  // end for
  }    // end while
}  // end method

// -----------------------------------------------------------------------------

void Circuit::walkThroughWorstSlackPath(Vcell *seed, EdgeType edgeType,
                                        WalkerCallback callback) {
  // Queue.
  queue<Vcell *> q;

  // Put seed cell into the queue.
  q.push(seed);

  // Walk
  while (!q.empty()) {
    Vcell *cell = q.front();
    q.pop();

    if (cell->actualInstType->isSequential) break;

    (this->*callback)(cell);

    edgeType = (edgeType == RISE) ? FALL : RISE;

    double worstSlack = +numeric_limits<double>::max();
    int worstSlackIndex = -1;

    const int numPreviousCells = cell->previousCells.size();
    for (int i = 0; i < numPreviousCells; i++) {
      Vcell *driver = cell->previousCells[i];

      const double slack = driver->worstPathSlack[edgeType];
      if (slack < worstSlack) {
        worstSlack = slack;
        worstSlackIndex = i;
      }  // end if
    }    // end for

    if (worstSlackIndex != -1) q.push(cell->previousCells[worstSlackIndex]);
  }  // end while
}  // end method

// -----------------------------------------------------------------------------

void Circuit::walkCriticalPathForward(WalkerCallback callback) {
  const int numCells = criticalPath.size();
  for (int i = numCells - 1; i >= 0; i--) (this->*callback)(criticalPath[i]);
}  // end method

// -----------------------------------------------------------------------------

void Circuit::walkCriticalPathBackward(WalkerCallback callback) {
  const int numCells = criticalPath.size();
  for (int i = 0; i < numCells; i++) (this->*callback)(criticalPath[i]);
}  // end method

// -----------------------------------------------------------------------------

void Circuit::walkTopCriticalCells(WalkerCallback callback) {
  const int topCells = topCriticalCells.size();
  for (int i = 0; i < topCells; i++) {
    (this->*callback)(topCriticalCells[i]);
    // cout << "Celula" << topCriticalCells[i]->instName << "\t numero " << i <<
    // endl;
  }  // end for
}  // end method

// -----------------------------------------------------------------------------

void Circuit::walkFollowingBacktrackPointers(const int netIndex,
                                             const EdgeType edgeType,
                                             WalkerCallback callback) {
  int n = netIndex;
  EdgeType e = edgeType;

  while (true) {
    const TimingNet &net = timingNets[n];
    if (net.depth == 0) break;

    (this->*callback)(net.driver);

    const TimingNetState &netstate = getTimingNetState(n);
    n = timingArcs[netstate.backtrack[e]].driver;
    e.reverse();
  }  // end while
}  // end method

// -----------------------------------------------------------------------------

void Circuit::walkCloadLargeToSmall(WalkerCallback callback) {
  const int numCells = cloadOrdered.size();
  for (int i = 0; i < numCells; i++) {
    (this->*callback)(cloadOrdered[i]);
    // cout << cloadOrdered[i]->actualLoad << endl;
  }  // end for
}  // end method

// -----------------------------------------------------------------------------

void Circuit::walkCloadSmallToLarge(WalkerCallback callback) {
  const int numCells = cloadOrdered.size();
  for (int i = numCells - 1; i >= 0; i--) {
    (this->*callback)(cloadOrdered[i]);
    // cout << cloadOrdered[i]->actualLoad << endl;
  }  // end for
}  // end method

// -----------------------------------------------------------------------------

void Circuit::sizingVaiVem1() {
  for (int i = 0; i < 500; i++) {
    walkBackward(&Circuit::stepperVaiVem1);
    walkForward(&Circuit::stepperVaiVem1);
    // walkBackward( &Circuit::sizingGraci );
    if (worstSlack > 0) {
      cout << "Iteracao ============================ " << i << endl << endl;
      break;
    }
  }
}  // end method

// -----------------------------------------------------------------------------

void Circuit::walkForwardDepth(WalkerCallback callback) {
  /*multimap<int,Vcell*> m;

const int numNodes = icells.size();
for ( int i = 0; i < numNodes; i++ ) {
Vcell * cell = icells[i];
m.insert( make_pair( cell->logicalDepth, cell ) );
} // end for

multimap<int,Vcell*>::iterator it;
for ( it = m.begin(); it != m.end(); it++ )
(*callback)(it->second);
*/

  // changed to use depthSortedCells vector - Tiago Reimann (21/08/2012)
  vector<Vcell *>::iterator it;
  for (it = depthSortedCells.begin(); it != depthSortedCells.end(); it++)
    (this->*callback)(*it);

}  // end method

// -----------------------------------------------------------------------------

void Circuit::walkBackwardDepth(WalkerCallback callback) {
  /*	multimap<int,Vcell*> m;

   const int numNodes = icells.size();
   for ( int i = 0; i < numNodes; i++ ) {
   Vcell * cell = icells[i];
   m.insert( make_pair( cell->logicalDepth, cell ) );
   } // end for

   multimap<int,Vcell*>::reverse_iterator it;
   for ( it = m.rbegin(); it != m.rend(); it++ )
   (*callback)(it->second);
   */

  // changed to use depthSortedCells vector - Tiago Reimann (21/08/2012)
  vector<Vcell *>::reverse_iterator it;
  for (it = depthSortedCells.rbegin(); it != depthSortedCells.rend(); it++)
    (this->*callback)(*it);

}  // end method

// -----------------------------------------------------------------------------

void Circuit::stepperVaiVem1(Vcell *cell) {
  // Print gate name
  // debug( "graci", endl << endl << "Celula: " << cell->instName << " do tipo "
  // << cell->instType << endl << endl );

  if (cell->actualInstType->isSequential) {
    // cout << "Cell sequencial \n \n";
    return;
  }

  double initialWorstSlack = getWorstSlack();
  // debug( "graci", "O slack Ã© " << initialWorstSlack << endl );
  // cout << "O slack da celula " << cell->instName << " eh " <<
  // initialWorstSlack << endl << endl;

  const int originalTypeIndex = cell->actualInstTypeIndex;

  const double CLoad = cell->actualLoad;
  // cout << "Cload da celula = " << CLoad << endl;

  const int numCells = orgCells.oCells[cell->footprintIndex].cells.size();
  // cout << "numCells = " << numCells << endl;

  int bestCell = -1;
  double bestSlack = -numeric_limits<double>::max();

  for (int j = 0; j < numCells; j++) {
    // while ( j < numCells && worstSlack <= initialWorstSlack ) {
    // while ( j < numCells ) {
    // debug( "graci", "Worst slack atual " << worstSlack << " e o worst slack
    // anterior " << initialWorstSlack << endl << endl );

    const LibParserCellInfo &cellinfo =
        orgCells.oCells[cell->footprintIndex].cells[j];

    double maxPinCap = 0;

    const int numPins = cellinfo.pins.size();
    // cout << "O numero de pinos Ã© " << numPins << endl << endl;

    for (int k = 0; k < numPins; k++) {
      const LibParserPinInfo &pininfo = cellinfo.pins[k];
      if (!pininfo.isInput) maxPinCap = pininfo.maxCapacitance;
      // cout << "Maxima capacitancia = " << maxPinCap << endl;
    }  // end for

    // if (true || maxPinCap >= CLoad) {
    if (maxPinCap >= CLoad) {
      updateCellType(cell, j);
      // calcTiming();
      // coneTiming2(cell);
      updateTiming(cell);
      // calcTimingViol();

      // if ( worstSlack > initialWorstSlack )
      //	break;
      // cout << "Slack atual " << worstSlack << "/t Best slack " << bestSlack
      // << " /t Slack inicial " << initialWorstSlack << endl;
      if (getWorstSlack() > bestSlack && worstSlack >= initialWorstSlack) {
        // if ( worstSlack > bestSlack ) {
        bestCell = j;
        bestSlack = getWorstSlack();
        // break;
      }  // end if

    }  // end if

  }  // end for

  if (bestCell != -1) {
    updateCellType(cell, bestCell);
    // cout << "Best Cell " << bestCell << endl;
    // debug( "graci", "Celula " << cell->instName << " tipo " << cell->instType
    // << "\n\n" ); calcTiming(); coneTiming2(cell);
    updateTiming(cell);
    // calcTimingViol();
  }  // end if
  /*
   else {
   //UNDO
   //debug( "graci", "Best cell = -1 \n" );
   updateCellType( cell, originalTypeIndex );
   coneTiming(cell);
   calcTimingViol();
   //cout << "Slack gerado " << worstSlack << endl;
   } // end if
   */

  debug("graci",
        "\rO slack Ã© " << getWorstSlack() << "\t" << bestCell << " \n");
  debug("flach",
        "\rO slack Ã© " << getWorstSlack() << "\t" << bestCell << " \n");

  if (worstSlack >= 0) {
    // cerr << "Entrou no return acabou aqui. \n \n ";
    stopWalking = true;
  }

}  // end method

void Circuit::stepperVaiVem2(Vcell *cell) {
  if (!cell->actualInstType->isSequential && !cell->dontTouch) {
    // getWorstSlack();
    // double initialWorstSlack = worstSlack;
    double initialWorstSlack = getWorstSlack();
    // debug( "graci", "O slack Ã© " << initialWorstSlack << endl );
    // cout << "O slack da celula " << cell->instName << " eh " <<
    // initialWorstSlack << endl << endl;

    const int originalTypeIndex = cell->actualInstTypeIndex;

    const double CLoad = cell->actualLoad;
    // cout << "Cload da celula = " << CLoad << endl;

    const int numCells = orgCells.oCells[cell->footprintIndex].cells.size();
    // cout << "numCells = " << numCells << endl;

    int bestCell = -1;
    double bestSlack = -numeric_limits<double>::max();

    for (int j = 0; j < numCells; j++) {
      // while ( j < numCells && worstSlack <= initialWorstSlack ) {
      // while ( j < numCells ) {
      // debug( "graci", "Worst slack atual " << worstSlack << " e o worst slack
      // anterior " << initialWorstSlack << endl << endl );

      const LibParserCellInfo &cellinfo =
          orgCells.oCells[cell->footprintIndex].cells[j];

      double maxPinCap = 0;

      const int numPins = cellinfo.pins.size();
      // cout << "O numero de pinos Ã© " << numPins << endl << endl;

      for (int k = 0; k < numPins; k++) {
        const LibParserPinInfo &pininfo = cellinfo.pins[k];
        if (!pininfo.isInput) maxPinCap = pininfo.maxCapacitance;
        // cout << "Maxima capacitancia = " << maxPinCap << endl;
      }  // end for

      // if (true || maxPinCap >= CLoad) {
      if (maxPinCap >= CLoad) {
        updateCellType(cell, j);
        updateTiming(cell);

        // computeCellTiming( cell, &cellinfo, cell->outputRiseDelay,
        // cell->outputFallDelay, cell->outputRiseSlew, cell->outputFallSlew,
        // cell->delays );

        if (getWorstSlack() > bestSlack &&
            getWorstSlack() >= initialWorstSlack) {
          // if ( worstSlack > bestSlack ) {
          bestCell = j;
          bestSlack = getWorstSlack();
          // break;
        }  // end if

      }  // end if

    }  // end for

    if (bestCell != -1) {
      updateCellType(cell, bestCell);
      cell->changed = true;
      // cout << "Best Cell " << bestCell << endl;
      // debug( "graci", "Celula " << cell->instName << " tipo " <<
      // cell->instType << "\n\n" ); calcTiming();
      updateTiming(cell);
      // cell->changed = true;
      // calcTimingViol();
    }  // end if

    if (getWorstSlack() < initialWorstSlack) {
      // UNDO
      // debug( "graci", "Best cell = -1 \n" );
      updateCellType(cell, originalTypeIndex);
      updateTiming(cell);
      // cout << "Slack gerado " << worstSlack << endl;
    }  // end if

    debug("graci", "\rO slack Ã© " << getWorstSlack() << "\t" << bestCell
                                   << "\tLeakage " << totalLeakage << " \n");
    debug("flach",
          "\rO slack Ã© " << getWorstSlack() << "\t" << bestCell << " \n");

    if (getWorstSlack() >= 0) {
      // cerr << "Entrou no return acabou aqui. \n \n ";
      stopWalking = true;
    }  // end if

  }  // end if

}  // end method

// -----------------------------------------------------------------------------

void Circuit::stepperSetGlobalCriticality(Vcell *cell) {
  cell->globalCriticalityOutputPinFall = 0;
  cell->globalCriticalityOutputPinRise = 0;

  const int n = cell->sinkNetIndex;
  const TimingNetState &netstate = getTimingNetState(n);

  cell->globalCriticalityOutputPinFall =
      timingRequiredTime[n].getFall() - netstate.arrivalTime.getFall();
  cell->globalCriticalityOutputPinRise =
      timingRequiredTime[n].getRise() - netstate.arrivalTime.getRise();

}  // end method

// -----------------------------------------------------------------------------

void Circuit::stepperSetSlewTarget(Vcell *cell) {
  // Rise and fall slew target is initially equal to max_transition available in
  // the cell library
  cell->fallSlewTarget = maxTransition;
  cell->riseSlewTarget = maxTransition;

  cell->deltaFallSlewTarget = numeric_limits<double>::max();
  cell->deltaRiseSlewTarget = numeric_limits<double>::max();

}  // end method

// -----------------------------------------------------------------------------

void Circuit::createInputSlewEstimates(Vcell *cell) {
  const int numPreviousCells = cell->previousCells.size();

  for (int i = 0; i < numPreviousCells; i++) {
    Vcell *driver = cell->previousCells[i];
    driver->slewFallEstimative = (alpha * driver->fallSlewTarget) -
                                 (1 - alpha) * (driver->outputFallSlew);
    driver->slewRiseEstimative = (alpha * driver->riseSlewTarget) -
                                 (1 - alpha) * (driver->outputRiseSlew);
  }  // end for

}  // end method

// -----------------------------------------------------------------------------

void Circuit::stepperSetLocalCriticality(Vcell *cell) {
  // if (cell->actualInstType->isSequential) {
  // return;
  // }

  // double alpha = 1;
  double max_change = 50.0;

  const int numPreviousCells = cell->previousCells.size();
  cell->localFallCriticality = -numeric_limits<double>::max();
  cell->localRiseCriticality = -numeric_limits<double>::max();
  double gammaFallVariable =
      ((cell->outputFallSlew) /
       (cell->actualFallSlack));  // abs((cell->outputFallSlew) /
                                  // (cell->actualFallSlack))
  double gammaRiseVariable =
      ((cell->outputRiseSlew) /
       (cell->actualRiseSlack));  // abs((cell->outputRiseSlew) /
                                  // (cell->actualRiseSlack))

  for (int i = 0; i < numPreviousCells; i++) {
    Vcell *driver = cell->previousCells[i];

    // usar required time e o arrival
    cell->localRiseCriticality =
        max(max((cell->actualRiseSlack) - (driver->actualRiseSlack), 0.0),
            cell->localRiseCriticality);
    // cout <<  "Cell " << cell->instName << " driver " << driver->instName << "
    // RISE slack drive " << driver->actualRiseSlack << endl;
    cell->localFallCriticality =
        max(max((cell->actualFallSlack) - (driver->actualFallSlack), 0.0),
            cell->localFallCriticality);
    // cout << "FALL slack drive " << driver->actualFallSlack << endl;

  }  // end for

  // Calculate considering FALL transition

  // cout << "Cell actual Fall " << cell->actualFallSlack << " criticalidade
  // global fall " << cell->globalCriticalityOutputPinFall << endl;

  if (((cell->actualFallSlack) < 0) && (cell->localFallCriticality == 0)) {
    cell->deltaFallSlewTarget = -min(
        alpha * gammaFallVariable * abs(cell->globalCriticalityOutputPinFall),
        max_change);
  }  // end if
  else {
    cell->globalCriticalityOutputPinFall =
        max((cell->actualFallSlack), cell->localFallCriticality);
    cell->deltaFallSlewTarget = min(
        alpha * gammaFallVariable * abs(cell->globalCriticalityOutputPinFall),
        max_change);
  }  // end else
  cell->fallSlewTarget = cell->fallSlewTarget + cell->deltaFallSlewTarget;

  // Calculate considering RISE transition
  if (((cell->actualRiseSlack) < 0) && (cell->localRiseCriticality == 0)) {
    cell->deltaRiseSlewTarget = -min(
        alpha * gammaRiseVariable * abs(cell->globalCriticalityOutputPinRise),
        max_change);
  } else {
    cell->globalCriticalityOutputPinRise =
        max((cell->actualRiseSlack), cell->localRiseCriticality);
    cell->deltaRiseSlewTarget = min(
        alpha * gammaRiseVariable * abs(cell->globalCriticalityOutputPinRise),
        max_change);
  }  // end else
  cell->riseSlewTarget = cell->riseSlewTarget + cell->deltaRiseSlewTarget;

  const int numOCells = orgCells.oCells.size();

  for (int j = 0; j < (numOCells); ++j) {
    if (cell->footprintIndex == j) {
      cell->fallSlewTarget =
          max(cell->fallSlewTarget, (orgCells.oCells[j].minimumFallSlew));
      cell->riseSlewTarget =
          max(cell->riseSlewTarget, (orgCells.oCells[j].minimumRiseSlew));
      cell->fallSlewTarget =
          min(cell->fallSlewTarget, (orgCells.oCells[j].maximumFallSlew));
      cell->riseSlewTarget =
          min(cell->riseSlewTarget, (orgCells.oCells[j].maximumRiseSlew));
    }  // end if
  }    // end for

}  // end method

// -----------------------------------------------------------------------------

void Circuit::stepperSetLocalCriticalityGC(Vcell *cell) {
  double minimumSlackPreviousCellsFall = numeric_limits<double>::max();
  double minimumSlackPreviousCellsRise = numeric_limits<double>::max();

  const int numPreviousCells = cell->previousCells.size();
  cell->localFallCriticality = -numeric_limits<double>::max();
  cell->localRiseCriticality = -numeric_limits<double>::max();

  for (int i = 0; i < numPreviousCells; i++) {
    Vcell *driver = cell->previousCells[i];

    // usar required time e o arrival
    minimumSlackPreviousCellsFall =
        min(driver->globalCriticalityOutputPinFall,
            minimumSlackPreviousCellsFall);  // usa criticalidade global ou
                                             // actualslack?
    minimumSlackPreviousCellsRise = min(driver->globalCriticalityOutputPinRise,
                                        minimumSlackPreviousCellsRise);

    // using actual slack
    // minimumSlackPreviousCellsFall = min(driver->actualFallSlack,
    // minimumSlackPreviousCellsFall) ; minimumSlackPreviousCellsRise =
    // min(driver->actualRiseSlack, minimumSlackPreviousCellsRise) ;

  }  // end for

  cell->localFallCriticality =
      max(cell->globalCriticalityOutputPinFall - minimumSlackPreviousCellsFall,
          0.0);

  cell->localRiseCriticality =
      max(cell->globalCriticalityOutputPinRise - minimumSlackPreviousCellsRise,
          0.0);

}  // end method

// -----------------------------------------------------------------------------

void Circuit::stepperRefiningSlewTargets(Vcell *cell) {
  double max_change = 50.0;

  // double gammaFallVariable = ((cell->outputFallSlew) /
  // (cell->globalCriticalityOutputPinFall));
  // double gammaRiseVariable = ((cell->outputRiseSlew) /
  // (cell->globalCriticalityOutputPinRise));

  // Held says that he used a small constant to gamma
  double gammaFallVariable = 0.8;
  double gammaRiseVariable = 0.8;

  if (((cell->globalCriticalityOutputPinFall) < 0) &&
      (cell->localFallCriticality == 0)) {
    cell->deltaFallSlewTarget =
        -min(abs(alpha * gammaFallVariable *
                 abs(cell->globalCriticalityOutputPinFall)),
             max_change);
  }  // end if
  else {
    cell->globalCriticalityOutputPinFall =
        max((cell->globalCriticalityOutputPinFall), cell->localFallCriticality);
    cell->deltaFallSlewTarget =
        min(abs(alpha * gammaFallVariable *
                abs(cell->globalCriticalityOutputPinFall)),
            max_change);
  }  // end else

  cell->fallSlewTarget = cell->fallSlewTarget + cell->deltaFallSlewTarget;

  //=====================================================================================================================//
  // Calculate considering RISE transition

  if (((cell->globalCriticalityOutputPinRise) < 0) &&
      (cell->localRiseCriticality == 0)) {
    cell->deltaRiseSlewTarget =
        -min(abs(alpha * gammaRiseVariable *
                 abs(cell->globalCriticalityOutputPinRise)),
             max_change);
  } else {
    cell->globalCriticalityOutputPinRise =
        max((cell->globalCriticalityOutputPinRise), cell->localRiseCriticality);
    cell->deltaRiseSlewTarget =
        min(abs(alpha * gammaRiseVariable *
                abs(cell->globalCriticalityOutputPinRise)),
            max_change);
  }  // end else

  cell->riseSlewTarget = cell->riseSlewTarget + cell->deltaRiseSlewTarget;

  const int numOCells = orgCells.oCells.size();

  for (int j = 0; j < (numOCells); ++j) {
    if (cell->footprintIndex == j) {
      // cout << "Cell " << cell->instName << " slew target FALL " <<
      // cell->fallSlewTarget << " minimum FALL " <<
      // orgCells.oCells[j].minimumFallSlew << endl;

      // cout << "Cell " << cell->instName << " slew target RISE " <<
      // cell->riseSlewTarget << " minimum RISE " <<
      // orgCells.oCells[j].minimumRiseSlew << endl;

      cell->fallSlewTarget =
          max(cell->fallSlewTarget, (orgCells.oCells[j].minimumFallSlew));
      cell->riseSlewTarget =
          max(cell->riseSlewTarget, (orgCells.oCells[j].minimumRiseSlew));
      cell->fallSlewTarget =
          min(cell->fallSlewTarget, (orgCells.oCells[j].maximumFallSlew));
      cell->riseSlewTarget =
          min(cell->riseSlewTarget, (orgCells.oCells[j].maximumRiseSlew));
    }  // end if
  }    // end for

}  // end method

// -----------------------------------------------------------------------------

void Circuit::stepperSetCellSlewTarget(Vcell *cell) {
  if (cell->actualInstType->isSequential || cell->instName == "inputDriver") {
    return;
  }

  createInputSlewEstimates(cell);

  const int numCandidateCells =
      orgCells.oCells[cell->footprintIndex].cells.size();

  vector<pair<double, double>> delays = cell->delays;

  double maxRiseSlew;
  double maxFallSlew;

  double maxRiseDelay;
  double maxFallDelay;

  double outRiseDelay = 0.0;
  double outFallDelay = 0.0;

  double outRiseSlew = 0.0;
  double outFallSlew = 0.0;

  const double xOutputLoad = cell->actualLoad;
  double yInputRiseSlew = -numeric_limits<double>::max();
  double yInputFallSlew = -numeric_limits<double>::max();

  const int numPreviousCells = cell->previousCells.size();

  bool cellChanged = false;
  int i = 0;

  for (i = 0; i < numCandidateCells; i++) {
    LibParserCellInfo *cellInst =
        &orgCells.oCells[cell->footprintIndex].cells[i];
    int j = 0;
    int slewOk = 0;
    int k = 0;
    int timingArcs = cellInst->timingArcs.size();

    for (k = 0; k < cellInst->timingArcs.size(); ++k) {
      if (cellInst->timingArcs[k].toPin != "o") continue;

      for (int l = 0; l < numPreviousCells; l++) {
        Vcell *driver = cell->previousCells[l];

        for (int m = 0; m < cell->pinNetPairs.size(); ++m) {
          if (((driver->returnNetConnectedToPin("o")) ==
               cell->pinNetPairs[m].second) &&
              cellInst->timingArcs[k].fromPin == cell->pinNetPairs[m].first) {
            // cout << "Net " << driver->returnNetConnectedToPin("o") << "
            // conectada no pino de saíde da celula " << driver->instName << "
            // conectada na celula " << cell->instName << " pino " <<
            // cell->pinNetPairs[m].first << " net " <<
            // cell->pinNetPairs[m].second << " timing arco da intância " <<
            // cellInst->timingArcs[k].fromPin << endl;
            yInputRiseSlew = max(driver->slewRiseEstimative, yInputRiseSlew);
            yInputFallSlew = max(driver->slewFallEstimative, yInputFallSlew);

          }  // end if
        }    // end for
      }      // end for

      //            for (j = 0; j < cell->pinNetPairs.size(); ++j) {
      //                if (cellInst->timingArcs[k].fromPin ==
      //                cell->pinNetPairs[j].first)
      //                    break;
      //            } // end for

      const LibParserTimingInfo &timingInfo = cellInst->timingArcs[k];

      // Rise and Fall Delay
      outRiseDelay =
          Circuit::lookup(timingInfo.riseDelay, xOutputLoad, yInputFallSlew);
      outFallDelay =
          Circuit::lookup(timingInfo.fallDelay, xOutputLoad, yInputRiseSlew);

      // Output Rise and Fall Slew
      outRiseSlew = Circuit::lookup(timingInfo.riseTransition, xOutputLoad,
                                    yInputRiseSlew);
      outFallSlew = Circuit::lookup(timingInfo.fallTransition, xOutputLoad,
                                    yInputFallSlew);

      if ((outRiseSlew <= cell->riseSlewTarget) &&
          (outFallSlew <= cell->fallSlewTarget))
        slewOk++;

      // Set timing values.
      maxRiseSlew = max(maxRiseSlew, outRiseSlew);
      maxFallSlew = max(maxFallSlew, outFallSlew);

      maxRiseDelay = max(maxRiseDelay, outRiseDelay);
      maxFallDelay = max(maxFallDelay, outFallDelay);

    }  // end for

    if (slewOk == timingArcs) {
      updateCellType(cell, i);
      // cout << "Celula " << cell->instName << " foi trocada pela instancia "
      // << i << endl;
      cellChanged = true;
      return;
    }  // end if

  }  // end for

  if (!cellChanged) {
    updateCellType(
        cell,
        i - 1);  // update to the last cell that contains the largest leakage
                 // cout << "UPDATE TO THE LARGER CELL" << endl;
  }              // end if

  /*
   const LibParserTimingInfo &timingInfo =
   arc.cell->actualInstType->timingArcs[arc.lut];

   // Find a best-fit for the slew target
   int bestCell = -1;
   double bestFit = numeric_limits<double>::max();

   for ( int i = initialCell; i < numCandidateCells; i++ ) {

   computeArcTiming(timingInfo, cell->slewEstimative, cell->actualLoad,
   arc.delay, arc.slew );
   // Compute slew target mismatch of the candidate cell itself.
   const double mismatchTotalSlew = fabs( cell->totalSlewTarget -
   (orgCells.oCells[i]->outputRiseSlew + orgCells.oCells[i]->outputFallSlew));
   const double diffMismatch =

   // recalcular o slew considerando o slewEstimative da célula anterior e para
   isso tem que usar o computeArcTiming -- abaixo copiei todo o método que
   utiliza o compute Arc pois ele atualiza o atraso do arco

   // Check if this is the lower mismatch
   if ( fabs(mismatch) < fabs(bestFit) ) {
   bestFit = mismatch;
   bestCell = i;
   } // end if
   } // end for

   // Change to the best cell candidate.
   updateCellType(cell, bestCell);

   */

}  // end method

void Circuit::stepperGreedySizing(Vcell *cell) {
  /*
   if (cell->actualInstType->isSequential || cell->instName == "inputDriver") {
   return;
   }

   const int numCandidateCells =
   orgCells.oCells[cell->footprintIndex].cells.size();

   //double previousTotalWorstSlack = cell->worstRiseSlack +
   cell->worstFallSlack; double previousTimingViol = timingViolationSlack;

   int bestCell = cell->actualInstTypeIndex;
   double bestTimingViol = timingViolationSlack;

   int i = 0;
   int m = 0;

   for ( i = 0; i < numCandidateCells; i++ ) {
   updateCellType( cell, i );

   updateTiming();


   //if (timingViolationSlack < previousTimingViol && (cell->worstRiseSlack +
   cell->worstFallSlack) < previousTotalWorstSlack){
   //if (timingViol <= previousTimingViol){
   if (m = 0) {
   bestTimingViol = timingViolationSlack;
   m++;
   } // end if
   //cout << "Timing violation " << timingViolationSlack << endl;
   if (timingViolationSlack < bestTimingViol) {
   bestCell = i;
   bestTimingViol = timingViolationSlack;

   } // end if

   //cout << "Celula " << cell->instName << " trocada por " << i << endl;

   //} // end if


   //actualize the values
   previousTotalWorstSlack = cell->worstRiseSlack + cell->worstFallSlack;
   previousTimingViol = timingViolationSlack;

   } // end for

   //cout << "Para a celula " << cell->instName << " a melhor instancia foi " <<
   bestCell << endl; updateCellType( cell, bestCell ); // update to the last
   cell that contains the largest leakage

   */

}  // end method

// -----------------------------------------------------------------------------

void Circuit::stepperRefineSlewTargets(Vcell *cell) {
  const int numPreviousCells = cell->previousCells.size();

  if (cell->actualInstType->isSequential || cell->instName == "inputDriver") {
    return;
  }

  double maxFallSlack = -numeric_limits<double>::max();
  double maxRiseSlack = -numeric_limits<double>::max();

  double factor = 0.7;

  for (int l = 0; l < numPreviousCells; l++) {
    Vcell *driver = cell->previousCells[l];

    driver->slewFallEstimative = (alpha * driver->fallSlewTarget) -
                                 (1 - alpha) * (driver->outputFallSlew);
    driver->slewRiseEstimative = (alpha * driver->riseSlewTarget) -
                                 (1 - alpha) * (driver->outputRiseSlew);

    maxFallSlack = max(driver->slewFallEstimative, maxFallSlack);
    maxRiseSlack = max(driver->slewRiseEstimative, maxRiseSlack);
  }  // end for

  cell->fallSlewTarget =
      cell->fallSlewTarget + factor * (maxFallSlack - cell->fallSlewTarget);
  cell->riseSlewTarget =
      cell->riseSlewTarget + factor * (maxRiseSlack - cell->riseSlewTarget);

}  // end method

// -----------------------------------------------------------------------------

void Circuit::computePathBoundaries() {
  pathHeads.clear();
  pathTails.clear();

  const int numNodes = icells.size();
  for (int i = 0; i < numNodes; i++) computePathBoundaries_CheckCell(icells[i]);

  const int numInputDrivers = graphRoot.nextCells.size();
  for (int i = 0; i < numInputDrivers; i++)
    computePathBoundaries_CheckCell(graphRoot.nextCells[i]);
}  // end method

// -----------------------------------------------------------------------------

void Circuit::computePathBoundaries_CheckCell(Vcell *cell) {
  // Check if at least one cell input is being driven by a primary input
  // or a sequential element.
  const int numPreviousNodes = cell->previousCells.size();
  for (int k = 0; k < numPreviousNodes; k++) {
    Vcell *driver = cell->previousCells[k];

    if (driver->dontTouch) {
      pathHeads.push_back(cell);
      break;
    }  // end if
  }    // end for

  // Check if the cell output is driving a primary output or a sequential
  // element.
  if (cell->portLoad > 0) {
    // Cell is driving a primary output.
    pathTails.push_back(cell);
  } else {
    const int numNextNodes = cell->nextCells.size();
    for (int k = 0; k < numNextNodes; k++) {
      Vcell *sink = cell->nextCells[k];

      if (sink->actualInstType->isSequential) {
        pathTails.push_back(cell);
        break;
      }  // end if
    }    // end for
  }      // end else
}  // end method

// -----------------------------------------------------------------------------

void Circuit::computeAvgNumberOfSinks() {
  avgNumberOfSinks = 0;

  const int numCells = icells.size();
  for (int i = 0; i < numCells; i++)
    avgNumberOfSinks += icells[i]->nextCells.size();

  avgNumberOfSinks /= numCells;
}  // end method

// -----------------------------------------------------------------------------

void Circuit::computeCellSizingOptions() {
  // [NOTE] HARD CODED
  // [NOTE] Assuming cells are already sorted by leakage.

  const int numFootprints = orgCells.oCells.size();

  timingLibraryCharacterization.resize(numFootprints);

  for (int i = 0; i < numFootprints; i++) {
    const OrdCells &footprint = orgCells.oCells[i];

    // cout << footprint.footprint << "\n";

    if ( true /* !footprint.cells.front().dontTouch && !footprint.cells.front().isSequential */ ) {
      const int numCellTypes = footprint.cells.size();

      LibCellGroupCharacterization &option = timingLibraryCharacterization[i];
      option.mappingVthSizeToCellTypeIndex.resize(3);  // [NOTE] hard coded
      option.cellCharacterization.resize(numCellTypes);

      for (int j = 0; j < numCellTypes; j++) {
        const LibParserCellInfo &cellType = footprint.cells[j];

        int vth = -1;

        const char vthName = cellType.name[footprint.footprint.size()];
        switch (vthName) {
          case 's':
            vth = 0;
            break;
          case 'm':
            vth = 1;
            break;
          case 'f':
            vth = 2;
            break;
          default:
            if (cellType.dontTouch || cellType.isSequential) {
              vth = 0;
            } else {
              cout << "[BUG] Invalid Vth :/\n";
              exit(1);
            }  // end else
        }      // end switch

        LibCellCharacterization &c = option.cellCharacterization[j];
        c.vth = vth;
        c.size = option.mappingVthSizeToCellTypeIndex[vth].size();

        option.mappingVthSizeToCellTypeIndex[vth].push_back(j);

        // cout << "\t" << cellType.name << "\t" << vth << "\n";
      }  // end for
    }    // end if
  }      // end for

  /*
   for ( int i = 0; i < numFootprints; i++ ) {
   const OrdCells &footprint = orgCells.oCells[i];
   cout << footprint.footprint << "\n";

   if ( !footprint.cells.front().dontTouch &&
   !footprint.cells.front().isSequential ) { const CellSizingOption &option =
   timingCellSizingOptions[i]; for ( int size = 0; size < 10; size++ ) { cout <<
   "\t"; for ( int vth = 0; vth < 3; vth++){
   //cout << option.option[vth][size] << "\t";
   cout << orgCells.oCells[i].cells[option.option[vth][size]].name << "\t";
   } // end for
   cout << "\n";
   } // end for
   } // end if
   } // end for
   */
}  // end method

// -----------------------------------------------------------------------------

void Circuit::computeCellDepths() {
  // Type used to store a node reference: cell, logical depth.
  typedef pair<Vcell *, int> Reference;

  // Queue.
  queue<Reference> q;

  // Clear logical depth.
  for (int i = 0; i < graphRoot.nextCells.size(); i++)
    graphRoot.nextCells[i]->logicalDepth = 0;  // input drivers

  const int numNodes = icells.size();
  for (int i = 0; i < numNodes; i++) {
    if (icells[i]->actualInstType->isSequential)
      icells[i]->logicalDepth = 0;
    else
      icells[i]->logicalDepth = -1;
  }  // end for

  // Clear max circuit depth.
  maxLogicalDepth = 0;

  // Put path heads in the queue.
  const int numHeadNodes = pathHeads.size();
  for (int i = 0; i < numHeadNodes; i++) q.push(make_pair(pathHeads[i], 1));

  // Walk
  while (!q.empty()) {
    Reference reference = q.front();
    q.pop();

    Vcell *cell = reference.first;
    const int logicalDepth = reference.second;

    if (cell->actualInstType->isSequential) continue;

    if (logicalDepth > cell->logicalDepth) {
      cell->logicalDepth = logicalDepth;
      maxLogicalDepth = max(maxLogicalDepth, logicalDepth);

      const int numNextCells = cell->nextCells.size();
      for (int i = 0; i < numNextCells; i++)
        q.push(make_pair(cell->nextCells[i], logicalDepth + 1));
    }  // end if

  }  // end while

  // Populate the logical depth sorted cell vector.
  multimap<int, Vcell *> m;

  for (int i = 0; i < graphRoot.nextCells.size(); i++)
    m.insert(make_pair(0, graphRoot.nextCells[i]));

  for (int i = 0; i < numNodes; i++) {
    Vcell *cell = icells[i];
    m.insert(make_pair(cell->logicalDepth, cell));
  }  // end for

  depthSortedCells.resize(m.size());
  cellIterator = depthSortedCells.begin();

  offsetCombinational = 0;
  int index = 0;
  for (multimap<int, Vcell *>::iterator it = m.begin(); it != m.end(); it++) {
    Vcell *cell = it->second;

    cell->depthIndex = index;
    depthSortedCells[index] = cell;

    index++;

    if (cell->logicalDepth == 0) {
      offsetCombinational++;
      ++cellIterator;
    }  // end if
  }    // end for

  offsetSequential = graphRoot.nextCells.size();

#ifndef NDEBUG
  // Check if for all cells a depth has been set.
  for (int i = 0; i < icells.size(); i++) {
    if (icells[i]->logicalDepth == -1)
      cerr << "[BUG] @ computeCellDepths(): untouched cell "
           << icells[i]->instName << " (" << icells[i]->instType << ").\n";
  }  // end for

  // Check if input drivers have a logical depth of zero.
  for (int i = 0; i < graphRoot.nextCells.size(); i++) {
    if (graphRoot.nextCells[i]->logicalDepth != 0)
      cerr << "[BUG] @ computeCellDepths(): Input driver cell has not zero "
              "depth ("
           << graphRoot.nextCells[i]->logicalDepth << ").\n";
  }  // end for

  // Check if all sequential cell have a logical depth of zero.
  for (int i = 0; i < icells.size(); i++) {
    if (icells[i]->actualInstType->isSequential && icells[i]->logicalDepth != 0)
      cerr
          << "[BUG] @ computeCellDepths(): Sequential cell has not zero depth ("
          << icells[i]->logicalDepth << ").\n";
  }  // end for

  // Check if max depth cell are only driving flip-flops or primary
  // outputs.
  for (int i = 0; i < icells.size(); i++) {
    Vcell *cell0 = icells[i];

    if (cell0->logicalDepth == maxLogicalDepth) {
      if (cell0->nextCells.size() > 0) {
        for (int k = 0; k < cell0->nextCells.size(); k++) {
          Vcell *cell1 = cell0->nextCells[k];

          if (!cell1->actualInstType->isSequential) {
            cerr << "[BUG] @ computeCellDepths(): max depth cell " << i
                 << " not connected to a sequential element.\n";
            cerr << "\tDriver: " << cell0->instName << "\t" << cell0->instType
                 << "\n";
            cerr << "\tSink..: " << cell1->instName << "\t" << cell1->instType
                 << "\n";
          }  // end if
        }    // end for
      } else if (cell0->portLoad == 0) {
        cerr << "[BUG] @ computeCellDepths(): max depth cell " << i
             << " not connected to a sequential element or primary output.\n";
      }  // end else-if
    }    // end if
  }      // end for

  // Check if cells are driving only cells with greater depth.
  for (int i = 0; i < icells.size(); i++) {
    Vcell *cell0 = icells[i];

    for (int k = 0; k < cell0->nextCells.size(); k++) {
      Vcell *cell1 = cell0->nextCells[k];

      if (!cell1->actualInstType->isSequential &&
          cell1->logicalDepth <= cell0->logicalDepth) {
        cerr << "[BUG] @ computeCellDepths(): cell " << i
             << " driving a cell with smaller depth.\n";
        cerr << "\tDriver: " << cell0->instName << "\t" << cell0->instType
             << "\n";
        cerr << "\tSink..: " << cell1->instName << "\t" << cell1->instType
             << "\n";
      }  // end if
    }    // end for
  }      // end for
#endif

}  // end method

// -----------------------------------------------------------------------------

void Circuit::computeCellReverseDepths() {
  // Type used to store a node reference: cell, logical depth.
  typedef pair<Vcell *, int> Reference;

  // Queue.
  queue<Reference> q;

  // Clear logical depth.
  for (int i = 0; i < graphRoot.nextCells.size(); i++)
    graphRoot.nextCells[i]->reverseLogicalDepth = -1;  // input drivers

  const int numNodes = icells.size();
  for (int i = 0; i < numNodes; i++) {
    icells[i]->reverseLogicalDepth = -1;
  }  // end for

  // Clear max circuit depth.
  maxReverseLogicalDepth = 0;

  // Put path tails in the queue.
  const int numTailNodes = pathTails.size();
  for (int i = 0; i < numTailNodes; i++) q.push(make_pair(pathTails[i], 1));

  // Walk
  while (!q.empty()) {
    Reference reference = q.front();
    q.pop();

    Vcell *cell = reference.first;
    const int reverseLogicalDepth = reference.second;

    if (reverseLogicalDepth > cell->reverseLogicalDepth) {
      cell->reverseLogicalDepth = reverseLogicalDepth;
      maxReverseLogicalDepth = max(maxReverseLogicalDepth, reverseLogicalDepth);

      if (cell->actualInstType->isSequential) continue;

      const int numPreviousCells = cell->previousCells.size();
      for (int i = 0; i < numPreviousCells; i++)
        q.push(make_pair(cell->previousCells[i], reverseLogicalDepth + 1));
    }  // end if

  }  // end while

#ifndef NDEBUG
  // Check if for all cells a depth has been set.
  for (int i = 0; i < icells.size(); i++) {
    if (icells[i]->reverseLogicalDepth == -1)
      cerr << "[BUG] @ computeCellReverseDepths(): untouched cell " << i
           << ".\n";
  }  // end for

  // Check if all sequential cell have a logical depth of zero.
  for (int i = 0; i < icells.size(); i++) {
    if (icells[i]->actualInstType->isSequential &&
        icells[i]->reverseLogicalDepth == 0)
      cerr << "[BUG] @ computeCellReverseDepths(): Sequential cell has zero "
              "reverse logical depth ("
           << icells[i]->logicalDepth << ").\n";
  }  // end for

  // Check if max depth cell are only driving flip-flops or primary
  // outputs.
  for (int i = 0; i < icells.size(); i++) {
    Vcell *cell0 = icells[i];

    if (cell0->reverseLogicalDepth == maxReverseLogicalDepth) {
      if (!cell0->actualInstType->isSequential &&
          cell0->instName != "inputDriver") {
        cerr << "[BUG] @ computeCellReverseDepths(): max depth cell "
             << cell0->instName << " (" << cell0->instType
             << ") should by a sequential cell or a input driver.\n";
      }  // end if
    }    // end if
  }      // end for

  // Check if cells are driving only cells with greater depth.
  for (int i = 0; i < icells.size(); i++) {
    Vcell *cell0 = icells[i];

    for (int k = 0; k < cell0->previousCells.size(); k++) {
      Vcell *cell1 = cell0->previousCells[k];

      if (!cell0->actualInstType->isSequential &&
          cell1->reverseLogicalDepth <= cell0->reverseLogicalDepth) {
        cerr << "[BUG] @ computeCellReverseDepths(): cell " << i
             << " driving a cell with smaller reverse logical depth.\n";
        cerr << "\tDriver: " << cell1->reverseLogicalDepth << "\t"
             << cell1->instName << "\t" << cell1->instType << "\n";
        cerr << "\tSink..: " << cell0->reverseLogicalDepth << "\t"
             << cell0->instName << "\t" << cell0->instType << "\n";
      }  // end if
    }    // end for
  }      // end for
#endif

}  // end method

// -----------------------------------------------------------------------------

void Circuit::computePseudoIndependentSets() {
  const int numNets = timingNets.size();
  const int numCandidateNets = numNets - timingOffsetToLevelOneNets;

  timingPseudIndependentSets.reserve(numNets);
  timingPseudIndependentSetPointers.reserve(numNets + 1);

  vector<int> touch(numNets, -1);
  vector<bool> convered(numNets, false);

  timingPseudIndependentSetPointers.push_back(0);
  timingNumPseudoIndependentSets = 0;

  vector<int> netIndexes(numCandidateNets);
  for (int i = 0; i < numCandidateNets; i++)
    netIndexes[i] = timingOffsetToLevelOneNets + i;

  // random_shuffle(netIndexes.begin(), netIndexes.end()); // [TODO] it seems to
  // be too slow, find alternative sort methods

  while (true) {
    // cerr << "Set " << timingNumPseudoIndependentSets << "\n";

    bool newset = false;

    // Put uncovered nets first.
    int skipCounter = 0;

    for (int i = 0; i < numCandidateNets; i++) {
      const int n = netIndexes[i];
      if (!convered[n]) {
        // Put it in the first places.
        swap(netIndexes[skipCounter++], netIndexes[i]);
      }  // end if
    }    // end for

    if (skipCounter == 0) break;

    // Create pseudo-independent set.
    for (int i = 0; i < numCandidateNets; i++) {
      const int n = netIndexes[i];

      if (touch[n] == timingNumPseudoIndependentSets) continue;

      convered[n] = true;

      // Process previous and current level timing arcs.
      const int k0 = timingArcPointers[n];
      const int k1 = timingArcPointers[n + 1];
      for (int k = k0; k < k1; k++) {
        const TimingArc &arc = timingArcs[k];

        touch[arc.driver] = timingNumPseudoIndependentSets;

        // Sink nets of driver nets.
        const int n0 = timingSinkNetPointers[arc.driver];
        const int n1 = timingSinkNetPointers[arc.driver + 1];
        for (int k = n0; k < n1; k++) {
          touch[timingSinkNets[k]] = timingNumPseudoIndependentSets;
        }  // end for
      }    // end for

      // Sink nets of this net.
      const int n0 = timingSinkNetPointers[n];
      const int n1 = timingSinkNetPointers[n + 1];
      for (int k = n0; k < n1; k++) {
        touch[timingSinkNets[k]] = timingNumPseudoIndependentSets;
      }  // end for

      // Add this net to the current set.
      timingPseudIndependentSets.push_back(n);
    }  // end for

    timingPseudIndependentSetPointers.push_back(
        timingPseudIndependentSets.size());
    timingNumPseudoIndependentSets++;
  }  // end while

  cerr << "#Sets = " << timingNumPseudoIndependentSets << "\n";

  /*
const int numSets = timingNumPseudoIndependentSets;
for ( int i = 0; i < numSets; i++ ) {
const int k0 = timingPseudIndependentSetPointers[i];
const int k1 = timingPseudIndependentSetPointers[i+1];

cerr << "Set " << i << "(" << (k1-k0) << "):\n";

for ( int k = k0; k < k1; k++ )
cerr << timingPseudIndependentSets[k] << " ";

cerr << "\n\n";
} // end for
*/
}  // end method

// -----------------------------------------------------------------------------

void Circuit::computeSpans() {
  const int numCombinationalCells =
      depthSortedCells.size() - offsetCombinational;

  timingSortedCellsBySpan.resize(numCombinationalCells);
  timingSpan.resize(timingNets.size(),
                    -1);  // -1 discard the cell itself from its span set

  vector<int> visited(timingNets.size(), -1);
  for (int n0 = timingOffsetToNetLevel.front();
       n0 < timingOffsetToNetLevel.back(); n0++) {
    queue<int> q;

    // Push all driver nets.
    //		const int k0 = timingArcPointers[n0];
    //		const int k1 = timingArcPointers[n0+1];
    //		for ( int k = k0; k < k1; k++ )
    //			q.push(timingArcs[k].sink);

    q.push(n0);

    while (!q.empty()) {
      const int n1 = q.front();
      q.pop();

      if (n1 < timingNumDummyNets || visited[n1] == n0) continue;

      visited[n1] = n0;
      timingSpan[n0]++;

      const int k0 = timingSinkNetPointers[n1];
      const int k1 = timingSinkNetPointers[n1 + 1];
      for (int k = k0; k < k1; k++) {
        q.push(timingSinkNets[k]);
      }  // end for
    }    // end while
  }      // end for

  multimap<int, int> m;

  int maxspan = 0;
  int avgspan = 0;
  for (int n = timingOffsetToNetLevel.front();
       n < timingOffsetToNetLevel.back(); n++) {
    const int span = timingSpan[n];

    maxspan = max(maxspan, span);
    avgspan += span;

    if (timingNets[n].depth > 0) m.insert(make_pair(span, n));
  }  // end for

  int counter = 0;
  for (multimap<int, int>::reverse_iterator it = m.rbegin(); it != m.rend();
       it++) {
    timingSortedCellsBySpan[counter++] = timingNets[it->second].driver;
  }  // end for

  const int numNets =
      timingOffsetToNetLevel.back() - timingOffsetToNetLevel.front();
  avgspan /= double(numNets);
  cout << "\tAvgSpan: " << avgspan << "\t" << (avgspan / double(numNets))
       << "\n";
  cout << "\tMaxSpan: " << maxspan << "\t" << (maxspan / double(numNets))
       << "\n";
}  // end method

// -----------------------------------------------------------------------------

void Circuit::computeReverseSpans() {
  const int numCombinationalCells =
      depthSortedCells.size() - offsetCombinational;

  timingReverseSpan.resize(timingNets.size(),
                           -1);  // -1 discard the cell itself from its span set

  vector<int> visited(timingNets.size(), -1);
  for (int n0 = timingOffsetToNetLevel.front();
       n0 < timingOffsetToNetLevel.back(); n0++) {
    queue<int> q;
    q.push(n0);

    while (!q.empty()) {
      const int n1 = q.front();
      q.pop();

      if (n1 < timingNumDummyNets || visited[n1] == n0) continue;

      visited[n1] = n0;
      timingReverseSpan[n0]++;

      const int k0 = timingArcPointers[n1];
      const int k1 = timingArcPointers[n1 + 1];
      for (int k = k0; k < k1; k++) {
        q.push(timingArcs[k].driver);
      }  // end for
    }    // end while
  }      // end for

  int maxspan = 0;
  int avgspan = 0;
  for (int n = timingOffsetToNetLevel.front();
       n < timingOffsetToNetLevel.back(); n++) {
    const int span = timingReverseSpan[n];
    maxspan = max(maxspan, span);
    avgspan += span;
  }  // end for

  const int numNets =
      timingOffsetToNetLevel.back() - timingOffsetToNetLevel.front();
  avgspan /= double(numNets);
  cout << "\tAvgSpan: " << avgspan << "\t" << (avgspan / double(numNets))
       << "\n";
  cout << "\tMaxSpan: " << maxspan << "\t" << (maxspan / double(numNets))
       << "\n";
}  // end method

// -----------------------------------------------------------------------------

void Circuit::computeLocalNets() {
  timingLocalNets.reserve(timingNets.size() * 7);
  timingLocalNetPointers.resize(timingNets.size() + 1);

  timingLocalNetsIncludingSideNets.reserve(timingNets.size() * 3);
  timingLocalNetPointersIncludingSideNets.resize(timingNets.size() + 1);

  timingForwardNets.reserve(timingNets.size() * 3);
  timingForwardNetPointers.resize(timingNets.size() + 1);

  for (int n = timingOffsetToNetLevel[1]; n < timingOffsetToNetLevel.back();
       n++) {
    map<int, set<int>> locals;
    map<int, set<int>> localsIncludingSideNets;
    map<int, set<int>> forwards;

    // Include this net into its local net set.
    locals[timingNets[n].depth].insert(n);
    localsIncludingSideNets[timingNets[n].depth].insert(n);

    {  // Put driver nets into local net set.
      const int k0 = timingArcPointers[n];
      const int k1 = timingArcPointers[n + 1];
      for (int k = k0; k < k1; k++) {
        const TimingArc &arc = timingArcs[k];
        const TimingNet &net = timingNets[arc.driver];

        assert(arc.driver >= timingNumDummyNets);

        locals[net.depth].insert(arc.driver);
        localsIncludingSideNets[net.depth].insert(arc.driver);

        // Side nets.
        const int s0 = timingSinkNetPointers[arc.driver];
        const int s1 = timingSinkNetPointers[arc.driver + 1];
        for (int s = s0; s < s1; s++) {
          const int sinkNetIndex = timingSinkNets[s];

          if (sinkNetIndex == n) continue;

          const TimingNet &net = timingNets[sinkNetIndex];
          forwards[net.depth].insert(sinkNetIndex);
          localsIncludingSideNets[net.depth].insert(sinkNetIndex);
        }  // end for
      }    // end for
    }      // end block

    {  // Put sink nets of this net into its local net set.
      const int k0 = timingSinkNetPointers[n];
      const int k1 = timingSinkNetPointers[n + 1];
      for (int k = k0; k < k1; k++) {
        const int sinkNetIndex = timingSinkNets[k];
        const TimingNet &net = timingNets[sinkNetIndex];
        locals[net.depth].insert(sinkNetIndex);
        localsIncludingSideNets[net.depth].insert(sinkNetIndex);
        forwards[net.depth].insert(sinkNetIndex);
      }  // end for
    }    // end block

    timingLocalNetPointers[n] = timingLocalNets.size();
    for (map<int, set<int>>::const_iterator itDepth = locals.begin();
         itDepth != locals.end(); itDepth++) {
      const set<int> &s = itDepth->second;
      for (set<int>::const_iterator itNet = s.begin(); itNet != s.end();
           itNet++)
        timingLocalNets.push_back(*itNet);
    }  // end for

    timingLocalNetPointersIncludingSideNets[n] =
        timingLocalNetsIncludingSideNets.size();
    for (map<int, set<int>>::const_iterator itDepth =
             localsIncludingSideNets.begin();
         itDepth != localsIncludingSideNets.end(); itDepth++) {
      const set<int> &s = itDepth->second;
      for (set<int>::const_iterator itNet = s.begin(); itNet != s.end();
           itNet++)
        timingLocalNetsIncludingSideNets.push_back(*itNet);
    }  // end for

    timingForwardNetPointers[n] = timingForwardNets.size();
    for (map<int, set<int>>::const_iterator itDepth = forwards.begin();
         itDepth != forwards.end(); itDepth++) {
      const set<int> &s = itDepth->second;
      for (set<int>::const_iterator itNet = s.begin(); itNet != s.end();
           itNet++)
        timingForwardNets.push_back(*itNet);
    }  // end for

  }  // end for

  timingLocalNetPointers.back() = timingLocalNets.size();
  timingLocalNetPointersIncludingSideNets.back() =
      timingLocalNetsIncludingSideNets.size();
  timingForwardNetPointers.back() = timingForwardNets.size();

}  // end method

// -----------------------------------------------------------------------------

void Circuit::computeLocalArcs() {
  timingLocalArcs.reserve(timingNets.size() * 20);
  timingLocalArcPointers.resize(timingNets.size() + 1);

  timingSideArcs.reserve(timingNets.size() * 3);
  timingSideArcPointers.resize(timingNets.size() + 1);

  for (int netIndex = timingOffsetToNetLevel[1];
       netIndex < timingOffsetToNetLevel.back(); netIndex++) {
    set<int> locals;
    set<int> sides;

    const int k0 = timingArcPointers[netIndex];
    const int k1 = timingArcPointers[netIndex + 1];
    for (int k = k0; k < k1; k++) {
      const TimingArc &arc = timingArcs[k];
      const int driverNetIndex = arc.driver;

      assert(driverNetIndex >= timingNumDummyNets);

      // Driver arcs.
      const int driver0 = timingArcPointers[driverNetIndex];
      const int driver1 = timingArcPointers[driverNetIndex + 1];
      for (int k = driver0; k < driver1; k++) {
        locals.insert(k);
      }  // end for

      // Sink arcs.
      const int sink0 = timingSinkArcPointers[driverNetIndex];
      const int sink1 = timingSinkArcPointers[driverNetIndex + 1];
      for (int k = sink0; k < sink1; k++) {
        locals.insert(timingSinkArcs[k]);

        // Side arcs
        if (timingArcs[timingSinkArcs[k]].sink != netIndex)
          sides.insert(timingSinkArcs[k]);
      }  // end for

    }  // end for

    // Process next level timing arcs.
    const int sink0 = timingSinkArcPointers[netIndex];
    const int sink1 = timingSinkArcPointers[netIndex + 1];
    for (int k = sink0; k < sink1; k++) {
      locals.insert(timingSinkArcs[k]);
    }  // end for

    timingLocalArcPointers[netIndex] = timingLocalArcs.size();
    for (set<int>::const_iterator itArc = locals.begin(); itArc != locals.end();
         itArc++)
      timingLocalArcs.push_back(*itArc);

    timingSideArcPointers[netIndex] = timingSideArcs.size();
    for (set<int>::const_iterator itArc = sides.begin(); itArc != sides.end();
         itArc++)
      timingSideArcs.push_back(*itArc);
  }  // end for

  timingLocalArcPointers.back() = timingLocalArcs.size();
  timingSideArcPointers.back() = timingSideArcs.size();

}  // end method

// -----------------------------------------------------------------------------

void Circuit::computeClouds() {
  /*	const int numCells = depthSortedCells.size();
   const int numCombinationCells = numCells - offsetCombinational;

   int cloudCounter = 0;

   for ( int index = offsetCombinational; index < numCells; index++ ) {
   Vcell * seedCell = depthSortedCells[index];

   if ( seedCell->cloud != -1 )
   continue;

   // Queue.
   queue<Vcell *> q;

   // Push the seed into the queue.
   q.push( seedCell );

   // Walk

   int cloudCardinality = 0;

   while (!q.empty()) {
   Vcell * cell = q.front();
   q.pop();

   if ( cell->actualInstType->isSequential || cell->dontTouch )
   continue;

   if ( cell->cloud == cloudCounter )
   continue;

   assert(cell->cloud == -1);

   cell->cloud = cloudCounter;
   cloudCardinality++;

   const int numNextCells = cell->nextCells.size();
   for ( int i = 0; i < numNextCells; i++ )
   if ( cell->nextCells[i]->cloud != cloudCounter)
   q.push( cell->nextCells[i] );

   const int numPreviousCells = cell->previousCells.size();
   for ( int i = 0; i < numPreviousCells; i++ )
   if ( cell->previousCells[i]->cloud != cloudCounter)
   q.push( cell->previousCells[i] );
   } // end while

   debug("flach", "Cloud " << cloudCounter << " has " << cloudCardinality << "
   cells (" << (100*cloudCardinality/double(numCombinationCells)) <<  "%).\n" );
   cloudCounter++;

   } // end for*/
}  // end method

// -----------------------------------------------------------------------------

void Circuit::computeExpectedDelays() {
  const double T = sdcInfos.clk_period;
  const int numCells = depthSortedCells.size();

  // Set the expected delay for path heads (input drivers and flip-flops).
  for (int index = 0; index < offsetCombinational; index++) {
    Vcell *cell = depthSortedCells[index];

    const double expectedDelay = T / cell->reverseLogicalDepth;

    cell->expectedDelay = expectedDelay;
    cell->expectedArrivalTime = expectedDelay;
  }  // end for

  // Set the expected delay for combinational cells.
  for (int index = offsetCombinational; index < numCells; index++) {
    Vcell *cell = depthSortedCells[index];

    double worstArrivalTime = 0;

    const int numPreviousCells = cell->previousCells.size();
    for (int k = 0; k < numPreviousCells; k++) {
      Vcell *driver = cell->previousCells[k];
      if (driver->expectedArrivalTime > worstArrivalTime)
        worstArrivalTime = driver->expectedArrivalTime;
    }  // end for

    cell->expectedDelay = (T - worstArrivalTime) / cell->reverseLogicalDepth;
    cell->expectedArrivalTime = worstArrivalTime + cell->expectedDelay;
  }  // end for

#ifndef NDEBUG
  const int numPathTails = pathTails.size();
  for (int i = 0; i < numPathTails; i++) {
    Vcell *cell = pathTails[i];

    if (!nearlyEqual(cell->expectedArrivalTime, T) &&
        (cell->reverseLogicalDepth == 1)) {
      cerr << "[BUG] @ computeExpectedDelays(): cell " << i
           << " has an expected arrival time different from T.\n";
      cerr << "\tClock Period.........: " << T << "\n";
      cerr << "\tExpected Arrival Time: " << cell->expectedArrivalTime << "\n";
    }  // end if
  }    // end for

#endif

}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateCriticalPath() {
  EdgeType edgeType =
      (timingWorstArrivalTime[FALL] > timingWorstArrivalTime[RISE]) ? FALL
                                                                    : RISE;

  int n = timingWorstArrivalTimeNet[edgeType];

  criticalPathEdgeType = edgeType;

  criticalPath.clear();
  criticalPath.reserve(timingNets[n].depth + 1);

  while (true) {
    const TimingNet &net = timingNets[n];

    if (net.depth == 0) break;

    criticalPath.push_back(net.driver);

    const TimingNetState &netstate = getTimingNetState(n);
    n = timingArcs[netstate.backtrack[edgeType]].driver;
    edgeType.reverse();
  }  // end while

  /*
   const int numCellsOnTheCriticalPath = criticalPath.size();

   criticalPathEnlarged.clear();
   criticalPathEnlarged.reserve(numCellsOnTheCriticalPath*3);

   for ( int i = 0; i < numCellsOnTheCriticalPath; i++ ) {
   const int n = criticalPath[i]->sinkNetIndex;

   const int k0 = timingSinkNetPointers[n];
   const int k1 = timingSinkNetPointers[n+1];
   for ( int k = k0; k < k1; k++ )
   criticalPathEnlarged.push_back(timingNets[timingSinkNets[k]].driver);
   } // end for
   */
}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateLeastCriticalPaths(const int numPaths) {
  // Sort tail nets by arrival times.
  typedef multimap<double, pair<int, EdgeType>> T;
  T m;

  const int numTailNets = timingTailNets.size();
  for (int i = 0; i < numTailNets; i++) {
    const int n = timingTailNets[i];
    const TimingNetState &netstate = getTimingNetState(n);

    for (int edge = 0; edge < 2; edge++)
      m.insert(make_pair(netstate.arrivalTime[edge], make_pair(n, edge)));
  }  // end for

  // Walk over the top most critical paths.
  topCriticalCells.clear();
  topCriticalCells.reserve(maxLogicalDepth * numPaths);

  int pathCounter = 0;
  for (T::iterator it = m.begin(); it != m.end() && pathCounter < numPaths;
       it++, pathCounter++) {
    EdgeType edgeType = it->second.second;
    int n = it->second.first;

    int lastDepth = 0;
    while (true) {
      const TimingNet &net = timingNets[n];

      if (net.depth < lastDepth) break;

      lastDepth = net.depth;

      if (net.driver->actualInstTypeIndex != 0)
        topCriticalCells.push_back(net.driver);
      // Reimann -----------------------------------------------
      const int next = net.driver->nextCells.size();
      for (int tt = 0; tt < next; ++tt)
        if (net.driver->nextCells[tt]->actualInstTypeIndex != 0)
          topCriticalCells.push_back(net.driver->nextCells[tt]);
      //-------------------------------------------------------

      const TimingNetState &netstate = getTimingNetState(n);

      n = timingArcs[netstate.backtrack[edgeType]].driver;
      edgeType.reverse();
    }  // end while

  }  // end for
}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateTopCriticalPaths(const int numPaths) {
  // Sort tail nets by arrival times.
  typedef multimap<double, pair<int, EdgeType>> T;
  T m;

  const int numTailNets = timingTailNets.size();
  for (int i = 0; i < numTailNets; i++) {
    const int n = timingTailNets[i];
    const TimingNetState &netstate = getTimingNetState(n);

    for (int edge = 0; edge < 2; edge++)
      m.insert(make_pair(netstate.arrivalTime[edge], make_pair(n, edge)));
  }  // end for

  // Walk over the top most critical paths.
  topCriticalCells.clear();
  topCriticalCells.reserve(maxLogicalDepth * numPaths);

  int pathCounter = 0;
  for (T::reverse_iterator it = m.rbegin();
       it != m.rend() && pathCounter < numPaths; it++, pathCounter++) {
    EdgeType edgeType = it->second.second;
    int n = it->second.first;

    while (true) {
      TimingNet &net = timingNets[n];

      if (net.depth == 0) break;

      topCriticalCells.push_back(net.driver);
      // Reimann -----------------------------------------------
      const int next = net.driver->nextCells.size();
      for (int tt = 0; tt < next; ++tt)
        topCriticalCells.push_back(net.driver->nextCells[tt]);
      //-------------------------------------------------------

      const TimingNetState &netstate = getTimingNetState(n);

      n = timingArcs[netstate.backtrack[edgeType]].driver;
      edgeType.reverse();
    }  // end while

  }  // end for
}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateTopLoadPaths() {
  Vcell *tmpCell;

  topCriticalCells.clear();

  int pathCounter = 0;
  for (int i = icells.size() - 1; i >= 0; --i) {
    tmpCell = icells[i];
    if (tmpCell->actualLoad > tmpCell->actualInstType->pins[0].maxCapacitance) {
      topCriticalCells.push_back(tmpCell);
      const int next = tmpCell->nextCells.size();
      for (int tt = 0; tt < next; ++tt)
        topCriticalCells.push_back(tmpCell->nextCells[tt]);
    }
  }  // end for
}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateCriticalPathCounter() {
  timingCriticalPathCounter.resize(timingNets.size());
  timingCriticalPathCounter.assign(timingCriticalPathCounter.size(), 0);

  // Update path counter.
  const int numTailNets = timingTailNets.size();
  for (int i = 0; i < numTailNets; i++) {
    for (int edge = 0; edge < 2; edge++) {
      int n = timingTailNets[i];
      EdgeType edgeType(edge);

      if (getNetSlack(n)[edge] >= 0) continue;

      while (true) {
        const TimingNet &net = timingNets[n];

        if (net.depth == 0) break;

        timingCriticalPathCounter[n]++;

        const TimingNetState &netstate = getTimingNetState(n);
        n = timingArcs[netstate.backtrack[edgeType]].driver;
        edgeType.reverse();
      }  // end while
    }    // end for
  }      // end for

  // Sort nets by path counter.
  multimap<int, int> m;
  for (int n = timingOffsetToNetLevel.front();
       n < timingOffsetToNetLevel.back(); n++) {
    m.insert(make_pair(timingCriticalPathCounter[n], n));
  }  // end for

  timingNetSortedByCriticalPathCounter.resize(timingOffsetToNetLevel.back() -
                                              timingOffsetToNetLevel.front());

  int index = 0;
  for (multimap<int, int>::const_reverse_iterator it = m.rbegin();
       it != m.rend(); it++) {
    timingNetSortedByCriticalPathCounter[index++] =
        make_pair(it->first, it->second);

    //		cout << "Net "
    //			<< setw(7) << it->second
    //			<< setw(7) << it->first
    //			<< "\n";

  }  // end for

}  // end method

// -----------------------------------------------------------------------------

void Circuit::computeNumberOfPathsPassingTrhuNets() {
  const int numNets = timingNets.size();

  // From inputs to outputs.
  for (int i = 0; i < numNets; i++) {
    TimingNet &net = timingNets[i];
    net.ipaths = 0;

    const int k0 = timingArcPointers[i];
    const int k1 = timingArcPointers[i + 1];
    for (int k = k0; k < k1; k++) {
      // Update arc timings.
      const TimingArc &arc = timingArcs[k];
      net.ipaths += timingNets[arc.driver].ipaths;
    }  // end for

    if (k0 == k1) {
      net.ipaths = 1;
    }  // end if
  }    // end for

  // From outputs to inputs.
  for (int i = numNets - 1; i >= 0; i--) {
    TimingNet &net = timingNets[i];
    net.opaths = 0;

    const int k0 = timingSinkNetPointers[i];
    const int k1 = timingSinkNetPointers[i + 1];
    for (int k = k0; k < k1; k++) {
      net.opaths += timingNets[timingSinkNets[k]].opaths;
    }  // end for

    if (k0 == k1) {
      net.opaths = 1;
    }  // end if

  }  // end for

  //	// From inputs to outputs.
  //	if (true) {
  //		ofstream file("path-count.report");
  //
  //		int minNumPaths = numeric_limits<int>::max();
  //		int maxNumPaths = 0;
  //
  //		for (int i = 0; i < numNets; i++) {
  //			const TimingNet &net = timingNets[i];
  //
  //			minNumPaths = min(minNumPaths, (net.ipaths*net.opaths));
  //			maxNumPaths = max(maxNumPaths, (net.ipaths*net.opaths));
  //
  //			file << timingNetName[i] << " "
  //					<< net.depth << " "
  //					<< net.ipaths << " "
  //					<< net.opaths << " "
  //					<< (net.ipaths*net.opaths) << "\n";
  //		} // end method
  //
  //		cout << "min num paths: " << minNumPaths << "\n";
  //		cout << "max num paths: " << maxNumPaths << "\n";
  //	} // end if
}  // end method

// -----------------------------------------------------------------------------

void Circuit::computeWeightedNumberOfPathsPassingTrhuNets() {
  const int numNets = timingNets.size();

  int maxLogicalDepth = 0;
  int maxReverseLogicalDepth = 0;
  for (int i = 0; i < numNets; i++) {
    TimingNet &net = timingNets[i];
    net.wipaths = 0;
    net.wopaths = 0;
    net.centrality = 0;

    const int depth = net.driver ? net.driver->logicalDepth : 0;
    const int reerseDepth = net.driver ? net.driver->reverseLogicalDepth : 0;
    maxLogicalDepth = std::max(maxLogicalDepth, depth);
    maxReverseLogicalDepth = std::max(maxReverseLogicalDepth, reerseDepth);
  }  // end for

  // From inputs to outputs.
  for (int i = 0; i < numNets; i++) {
    TimingNet &net = timingNets[i];
    net.wipaths = 0;

    const int k0 = timingArcPointers[i];
    const int k1 = timingArcPointers[i + 1];
    for (int k = k0; k < k1; k++) {
      // Update arc timings.
      const TimingArc &arc = timingArcs[k];
      net.wipaths += timingNets[arc.driver].wipaths;
    }  // end for

    if (k0 == k1 || (net.driver && net.driver->logicalDepth == 0)) {
      const float wdepth = net.driver ? net.driver->reverseLogicalDepth /
                                            float(maxReverseLogicalDepth)
                                      : 0;
      net.wipaths = wdepth;
    }  // end if
  }    // end for

  // From outputs to inputs.
  for (int i = numNets - 1; i >= 0; i--) {
    TimingNet &net = timingNets[i];
    net.wopaths = 0;

    const int k0 = timingSinkNetPointers[i];
    const int k1 = timingSinkNetPointers[i + 1];
    for (int k = k0; k < k1; k++) {
      net.wopaths += timingNets[timingSinkNets[k]].wopaths;
    }  // end for

    if (k0 == k1) {
      const float wdepth =
          net.driver ? net.driver->logicalDepth / float(maxLogicalDepth) : 0;
      net.wopaths = wdepth;
    }  // end if
  }    // end for

  //
  // Centrality
  //

  // Output Timing Arcs (tail)
  const int numArcs = timingArcs.size();
  for (int i = timingOffsetToExtraSequentialArcs; i < numArcs; i++) {
    const TimingArc &arc = timingArcs[i];
    TimingNet &net = timingNets[arc.driver];
    net.centrality = net.depth / float(maxLogicalDepth);
    // std::cout << "net depth " << net.depth  << " "
    //		<< net.driver->logicalDepth << " " << timingNetName[arc.driver]
    //<<
    //"\n";
  }  // end for

  for (int i = numNets - 1; i >= timingNumDummyNets; i--) {
    int sumDepths = 0;
    float centrality = timingNets[i].centrality;

    const int k0 = timingArcPointers[i];
    const int k1 = timingArcPointers[i + 1];

    const int q0 = timingSinkArcPointers[i];
    const int q1 = timingSinkArcPointers[i + 1];

    // Compute sum of driver timing arc lambdas.
    for (int k = k0; k < k1; k++) {
      const TimingArc &arc = timingArcs[k];
      sumDepths += timingNets[arc.driver].depth;
    }  // end for

    // Update driver arcs.
    const float sum = sumDepths;
    if (sum > 0) {
      for (int k = k0; k < k1; k++) {
        const TimingArc &arc = timingArcs[k];
        TimingNet &net = timingNets[arc.driver];
        net.centrality += centrality * (float(net.depth) / sum);
      }  // end for
    } else {
      const int numSinks = k1 - k0;
      for (int k = k0; k < k1; k++) {
        const TimingArc &arc = timingArcs[k];
        TimingNet &net = timingNets[arc.driver];
        net.centrality += centrality * (1.0f / numSinks);
      }  // end for
    }    // end else
  }      // end for

  // From inputs to outputs.
  if (true) {
    ofstream file("weighted-path-count.report");

    file << std::fixed << std::setprecision(5);

    int minNumPaths = numeric_limits<int>::max();
    int maxNumPaths = 0;

    for (int i = 0; i < numNets; i++) {
      const TimingNet &net = timingNets[i];

      minNumPaths = min(minNumPaths, (net.ipaths * net.opaths));
      maxNumPaths = max(maxNumPaths, (net.ipaths * net.opaths));

      file << std::setw(6) << net.depth << " " << std::setw(12) << net.wipaths
           << " " << std::setw(12) << net.wopaths << " " << std::setw(12)
           << (net.wipaths * net.wopaths) << " " << std::setw(12)
           << (net.centrality) << " " << std::setw(6) << timingNetName[i]
           << "\n";
    }  // end method

    cout << "min num paths: " << minNumPaths << "\n";
    cout << "max num paths: " << maxNumPaths << "\n";
  }  // end if
}  // end method

// -----------------------------------------------------------------------------

void Circuit::computeLogicalEffort_ReferenceSlew() {
  // HARD CODED
  const bool report = true;
  const int FOOTPRINT_INV = 0;
  const int PIN_INDEX = 1;

  const LibCellGroupCharacterization &option =
      timingLibraryCharacterization[FOOTPRINT_INV];
  const LibParserCellInfo &info =
      orgCells.oCells[FOOTPRINT_INV]
          .cells[option.mappingVthSizeToCellTypeIndex[0][0]];
  const LibParserTimingInfo &arc = info.timingArcs[0];

  const double Cin = info.pins[PIN_INDEX].capacitance;
  const double h = 1;
  const double load = h * Cin;

  for (int edge = 0; edge < 2; edge++) {
    const int maxIterations = 100;

    if (report) {
      cout << "Computing reference slew for "
           << (edge == RISE ? "RISE" : "FALL") << " transition...\n";
      cout << "reference gain: " << h << "\n";
      cout << "reference load: " << load << "\n";
    }  // end if

    double currSlew = 0;
    double prevSlew = 0;

    const LibParserLUT &lut =
        edge == RISE ? arc.riseTransition : arc.fallTransition;

    for (int i = 0; i < maxIterations; i++) {
      currSlew = lookup(lut, load, prevSlew);
      if (report) {
        cout << setw(2) << i << " " << currSlew << "\n";
      }  // end if
      if (approximatelyEqual(prevSlew, currSlew)) break;
      prevSlew = currSlew;
    }  // end for

    referenceSlew[edge] = currSlew;
  }  // end for

  if (report) {
    cout << "Reference Slew: " << referenceSlew << "\n";
  }  // end if

  //	cout << "[WARNING] Forcing reference slew to the input driver
  // slew...\n"; 	referenceSlew.set(40, 40);
  //
}  // end method

// -----------------------------------------------------------------------------

// Adapted from https://people.sc.fsu.edu/~jburkardt/cpp_src/llsq/llsq.cpp
// y = a * x + b

void Circuit::computeLogicalEffort_LinearLeastSquares(const vector<double> &x,
                                                      const vector<double> &y,
                                                      double &a, double &b) {
  const int n = x.size();

  double bot;
  int i;
  double top;
  double xbar;
  double ybar;

  if (n == 1) {
    //  Special case.
    a = 0.0;
    b = y[0];
  } else {
    //  Average X and Y.
    xbar = 0.0;
    ybar = 0.0;
    for (i = 0; i < n; i++) {
      xbar = xbar + x[i];
      ybar = ybar + y[i];
    }  // end for
    xbar = xbar / (double)n;
    ybar = ybar / (double)n;

    //  Compute Beta.
    top = 0.0;
    bot = 0.0;
    for (i = 0; i < n; i++) {
      top = top + (x[i] - xbar) * (y[i] - ybar);
      bot = bot + (x[i] - xbar) * (x[i] - xbar);
    }  // end for
    a = top / bot;
    b = ybar - a * xbar;
  }  // end else
}  // end method

// -----------------------------------------------------------------------------

void Circuit::computeLogicalEffort_LinearLeastSquaresError(
    const vector<double> &x, const vector<double> &y, const double a,
    const double b, double &residuum) {
  const int n = x.size();

  residuum = 0;
  for (int i = 0; i < n; i++) {
    const double estimate = a * x[i] + b;
    const double error = pow(y[i] - estimate, 2.0);
    // cout << "h=" << x[i] << " delay=" << y[i] << " estimate=" << (estimate)
    // << "\n";
    residuum += error;
  }  // end for
}  // end method

// -----------------------------------------------------------------------------

void Circuit::computeLogicalEffort_TimingArc(
    const LibParserLUT &lut, const double Cin, const double inputSlew,
    const vector<double> &gains, double &g, double &p, double &residuum) {
  const int N = gains.size();

  vector<double> delays(N);

  for (int i = 0; i < N; i++) {
    const double h = gains[i];
    const double load = h * Cin;
    delays[i] = lookup(lut, load, inputSlew);
    // cout << "h=" << h << " load=" << load << " delay=" << delays[i] << "\n";
  }  // end for

  computeLogicalEffort_LinearLeastSquares(gains, delays, g, p);
  computeLogicalEffort_LinearLeastSquaresError(gains, delays, g, p, residuum);
}  // end method

// -----------------------------------------------------------------------------

void Circuit::computeLogicalEffort_Report(ostream &out) {
  for (int footprint = 0; footprint < orgCells.oCells.size(); footprint++) {
    const LibCellGroupCharacterization &groupChar =
        timingLibraryCharacterization[footprint];

    for (int type = 0; type < groupChar.cellCharacterization.size(); type++) {
      const LibParserCellInfo &lcell = orgCells.oCells[footprint].cells[type];
      const LibCellCharacterization &cellChar =
          groupChar.cellCharacterization[type];

      for (int k = 0; k < cellChar.arcs.size(); k++) {
        const LibParserTimingInfo &larc = lcell.timingArcs[k];
        const LibArcCharacterization &arcChar = cellChar.arcs[k];

        for (int oedge = 0; oedge < 2; oedge++) {
          const int iedge = ((EdgeType)oedge).getReversed();
          if (arcChar.valid[oedge] == 0) continue;

          out << lcell.name << ":" << larc.fromPin << "->" << larc.toPin << ": "
              << "transition=" << (oedge == FALL ? "FALL" : "RISE") << " "
              << "vth=" << cellChar.vth << " "
              << "size=" << cellChar.size << "\n";

          out << setw(4) << "gain";
          out << setw(8) << "delay";
          out << setw(8) << "le_delay";
          out << "\n";

          const LibParserLUT &lut =
              (oedge == FALL) ? larc.fallDelay : larc.riseDelay;

          for (int i = 0; i < gains.size(); i++) {
            const double h = gains[i];
            const double load = h * arcChar.cin[iedge];
            const double delay = lookup(lut, load, referenceSlew[iedge]);
            const double le_delay = arcChar.p[oedge] + arcChar.g[oedge] * h;

            out << setw(4) << h;
            out << setw(8) << delay;
            out << setw(8) << le_delay;
            out << "\n";
          }  // end for
        }    // end for
      }      // end for

    }  // end for
  }    // end for
}  // end method

// -----------------------------------------------------------------------------

void Circuit::computeLogicalEffort() {
  const bool enableReport_LogicalEffortCharacterization = true;

  if (enableReport_LogicalEffortCharacterization) {
    reportFile_LogicalEffortCharacterization.open(
        "lib-characterization-le.report");
  }  // end if

  // Compute reference slew.
  computeLogicalEffort_ReferenceSlew();
  if (enableReport_LogicalEffortCharacterization)
    reportFile_LogicalEffortCharacterization
        << "Reference Slew: " << referenceSlew << "\n";

  // Define gains where the delay will be computed.
  const int N = 32 + 1;  // 0 ... 32
  gains.resize(N);
  for (int i = 0; i < N; i++) {
    gains[i] = i;
  }  // end for

  // Compute the logical effort for each timing arc.
  for (int footprint = 0; footprint < orgCells.oCells.size(); footprint++) {
    LibCellGroupCharacterization &groupChar =
        timingLibraryCharacterization[footprint];

    groupChar.arcs.resize(3);  // HARD CODED: number of vth groups
    for (int vth = 0; vth < 3; vth++) {
      groupChar.arcs[vth].resize(
          orgCells.oCells[footprint].cells[0].timingArcs.size());
      for (int k = 0; k < groupChar.arcs[vth].size(); k++) {
        groupChar.arcs[vth][k].g.set(0, 0);
        groupChar.arcs[vth][k].p.set(0, 0);
        groupChar.arcs[vth][k].valid.set(0, 0);
      }  // end for
    }    // end for

    for (int type = 0; type < orgCells.oCells[footprint].cells.size(); type++) {
      const LibParserCellInfo &lcell = orgCells.oCells[footprint].cells[type];
      LibCellCharacterization &cellChar = groupChar.cellCharacterization[type];

      if (enableReport_LogicalEffortCharacterization) {
        reportFile_LogicalEffortCharacterization
            << "Cell: " << lcell.name << " "
            << "vth=" << cellChar.vth << " "
            << "size=" << cellChar.size << "\n";
      }  // end if

      cellChar.arcs.resize(lcell.timingArcs.size());

      for (int k = 0; k < lcell.timingArcs.size(); k++) {
        const LibParserTimingInfo &larc = lcell.timingArcs[k];
        const int from = getLibPinIndexByName(lcell, larc.fromPin);
        const double Cin = lcell.pins[from].capacitance > 0
                               ? lcell.pins[from].capacitance
                               : defaultCin;

        LogicalEffort &arcLe = cellChar.arcs[k];
        arcLe.cin.set(Cin, Cin);
        arcLe.g.set(0, 0);
        arcLe.p.set(0, 0);
        arcLe.valid.set(0, 0);

        LogicalEffort &groupLe = groupChar.arcs[cellChar.vth][k];
        groupLe.cin += arcLe.cin;

        for (int edge = 0; edge < 2; edge++) {
          const LibParserLUT &lut =
              (edge == FALL) ? larc.fallDelay : larc.riseDelay;

          if (lut.loadIndices.size() > 0) {
            computeLogicalEffort_TimingArc(lut, Cin, referenceSlew[edge], gains,
                                           arcLe.g[edge], arcLe.p[edge],
                                           arcLe.residuum[edge]);

            arcLe.valid[edge] = 1;

            groupLe.g[edge] += arcLe.g[edge];
            groupLe.p[edge] += arcLe.p[edge];
            groupLe.residuum[edge] += arcLe.residuum[edge];
            groupLe.valid[edge]++;
          }  // end if
        }    // end for

        if (enableReport_LogicalEffortCharacterization) {
          reportFile_LogicalEffortCharacterization
              << "\t" << larc.fromPin << "->" << larc.toPin << ": "
              << "Cin=" << Cin << " "
              << "g=" << arcLe.g << " "
              << "p=" << arcLe.p << " "
              << "error=" << logicalEffort_MeanMSE(arcLe.residuum) << " "
              << "valid=" << arcLe.valid << "\n";
        }  // end if
      }    // end for arcs
    }      // end for cells

    if (enableReport_LogicalEffortCharacterization) {
      reportFile_LogicalEffortCharacterization
          << "Group: " << orgCells.oCells[footprint].footprint << "\n";
    }  // end if

    for (int vth = 0; vth < 3; vth++) {
      if (enableReport_LogicalEffortCharacterization) {
        reportFile_LogicalEffortCharacterization << "\tvth: " << vth << "\n";
      }  // end if

      for (int k = 0; k < groupChar.arcs[vth].size(); k++) {
        LogicalEffort &le = groupChar.arcs[vth][k];
        for (int edge = 0; edge < 2; edge++) {
          if (le.valid[edge] > 0) {
            le.g[edge] /= le.valid[edge];
            le.p[edge] /= le.valid[edge];
            le.residuum[edge] /= le.valid[edge];
            le.cin[edge] /= le.valid[edge];
          }  // end if
        }    // end for

        if (enableReport_LogicalEffortCharacterization) {
          const LibParserTimingInfo &larc =
              orgCells.oCells[footprint].cells[0].timingArcs[k];

          reportFile_LogicalEffortCharacterization
              << "\t" << larc.fromPin << "->" << larc.toPin << ": "
              << "Cin=" << le.cin << " "
              << "g=" << le.g << " "
              << "p=" << le.p << " "
              << "error=" << logicalEffort_MeanMSE(le.residuum) << " "
              << "valid=" << le.valid << "\n";
        }  // end if
      }    // end for
    }      // end for

  }  // end for groups (footprints)
}  // end method

// -----------------------------------------------------------------------------

void Circuit::printSigth(const string &filename) {
  ofstream file(filename.c_str());
  if (!file) {
    cerr << "[ERROR] Output sight file could not be created.\n";
    return;
  }  // end if

  // Constants.
  const int numCells = icells.size();

  // Create an auxiliary vector to keep cell x,y.
  vector<pair<double, double>> position(numCells);

  // Create an auxiliary vector to keep cell fanout/fanin ratios.
  vector<double> cellFanoutFaninRatios(numCells);

  // Create an auxiliary vector to keep cell organized by depth.
  vector<vector<int>> depths(maxLogicalDepth + 2);

  // Populate auxiliary vectors.
  double maxFanoutFaninRatio = 0;

  for (int i = 0; i < numCells; i++) {
    const int index =
        icells[i]->logicalDepth == -1 ? 0 : icells[i]->logicalDepth;

    depths[index].push_back(i);
    if (!icells[i]->dontTouch) {
      cellFanoutFaninRatios[i] = computeFanoutFaninRatio(icells[i]);
      maxFanoutFaninRatio = max(maxFanoutFaninRatio, cellFanoutFaninRatios[i]);
    }  // end if
  }    // end for

  // Calculate the maximum number of cells at a same depth.
  int maxNumCellsAtSameDepth = 0;
  for (int i = 0; i <= maxLogicalDepth; i++)
    maxNumCellsAtSameDepth = max(maxNumCellsAtSameDepth, (int)depths[i].size());

  // Calculated cell positions.
  const double scalex = 15;
  const double scaley = (scalex * maxNumCellsAtSameDepth) / maxLogicalDepth;

  const double dy = scaley;
  double y = 0;

  for (int i = 0; i <= maxLogicalDepth; i++) {
    const int cardinality = depths[i].size();

    const double dx =
        (double(maxNumCellsAtSameDepth) / double(cardinality)) * scalex;
    double x = dx / 2;

    for (int k = 0; k < cardinality; k++) {
      // cerr << depths[i][k] << "\n";
      position[depths[i][k]] = make_pair(x, y);
      x += dx;
    }  // end for
    y += dy;
  }  // end for

  // Generate Sight file.
  for (int i = 0; i < numCells; i++) {
    Vcell *cell0 = icells[i];
    const int ix0 = (int)floor(position[i].first + 0.5);
    const int iy0 = (int)floor(position[i].second + 0.5);

    if (cell0->dontTouch)
      file << "3 0 1 " << ix0 << " " << iy0 << " 10 10 black 1\n";
    else {
      const double delay = max(cell0->outputFallDelay, cell0->outputRiseDelay);
      const double targetDelay = (sdcInfos.clk_period / cell0->logicalDepth);

      const int w = 20;
      const int h = 20;

      int r, g, b;
      colorTemperature(delay / (sdcInfos.clk_period), r, g, b);
      file << "3 0 1 " << ix0 << " " << iy0 << " " << w << " " << h << " rgb "
           << r << " " << g << " " << b << "  1\n";

      /*
if ( delay > targetDelay )
file << "3 0 1 " << ix0 << " " << iy0 << " " << w << " " << h << " red 1\n";
else
file << "3 0 1 " << ix0 << " " << iy0 << " " << w << " " << h << " blue 1\n";
*/

    }  // end else

    // Print connections.
    const int numOutputs = cell0->nextCells.size();
    for (int j = 0; j < numOutputs; j++) {
      Vcell *cell1 = cell0->nextCells[j];
      if (cell1->dontTouch || cell0->dontTouch) continue;

      const int ix1 = (int)floor(position[cell1->vectorIndex].first + 0.5);
      const int iy1 = (int)floor(position[cell1->vectorIndex].second + 0.5);

      file << "2 0 0 " << ix0 << " " << iy0 << " " << ix1 << " " << iy1
           << " black\n";
    }  // end for
  }    // end for

  updateCriticalPath();
  for (int i = 0; i < criticalPath.size() - 1; i++) {
    Vcell *cell0 = criticalPath[i];
    Vcell *cell1 = criticalPath[i + 1];

    const int ix0 = (int)floor(position[cell0->vectorIndex].first + 0.5);
    const int iy0 = (int)floor(position[cell0->vectorIndex].second + 0.5);

    const int ix1 = (int)floor(position[cell1->vectorIndex].first + 0.5);
    const int iy1 = (int)floor(position[cell1->vectorIndex].second + 0.5);

    file << "2 0 3 " << ix0 << " " << iy0 << " " << ix1 << " " << iy1
         << " red\n";
  }

}  // end method

// -----------------------------------------------------------------------------

Vcell *Circuit::chooseCellRandomlyFromVector(vector<Vcell *> &v) {
  Vcell *cell = NULL;

  if (v.size() > 0) {
    do {
      const double randomNumber = rand() / double(RAND_MAX);
      const int randomIndex = (int)floor(randomNumber * (v.size() - 1) + 0.5);

      cell = v[randomIndex];
    } while ((cell->actualInstType->isSequential));
  }  // end if

  return cell;
}  // end method

// -----------------------------------------------------------------------------

double Circuit::computeAvgInputCapacitance(const LibParserCellInfo &cellinfo) {
  double avgInputCap = 0;

  const int numPins = cellinfo.pins.size();
  for (int k = 0; k < numPins; k++) {
    const LibParserPinInfo &pininfo = cellinfo.pins[k];

    if (pininfo.isInput) avgInputCap += pininfo.capacitance;
  }  // end for

  return avgInputCap /= numPins;  // considers all pins, including the output
                                  // pin
  // cout << "O nÃºmero de pinos Ã©: " << numPins << endl;
}  // end method

// -----------------------------------------------------------------------------

double Circuit::computeInputCapacitance(const LibParserCellInfo &cellinfo) {
  double inputCap = 0;

  const int numPins = cellinfo.pins.size();
  for (int k = 0; k < numPins; k++) {
    const LibParserPinInfo &pininfo = cellinfo.pins[k];

    if (pininfo.isInput) inputCap += pininfo.capacitance;
  }  // end for

  return inputCap;  // considering only 1 output pin
  // cout << "O nÃºmero de pinos Ã©: " << numPins << endl;
}  // end method

// -----------------------------------------------------------------------------

double Circuit::computeSumInputCapacitance(const LibParserCellInfo &cellinfo) {
  double sumInputCap = 0;

  const int numPins = cellinfo.pins.size();
  for (int k = 0; k < numPins; k++) {
    const LibParserPinInfo &pininfo = cellinfo.pins[k];

    if (pininfo.isInput) sumInputCap += pininfo.capacitance;
  }  // end for

  return sumInputCap;
}  // end method

// -----------------------------------------------------------------------------

bool Circuit::hasLoadViolationOnPreviousCells(Vcell *cell) {
  const int numPreviousCells = cell->previousCells.size();
  for (int i = 0; i < numPreviousCells; i++) {
    if (cell->previousCells[i]->actualLoad >
        cell->actualInstType->pins[0].maxCapacitance)
      return true;
  }  // end method
  return false;
}  // end method

// -----------------------------------------------------------------------------

double Circuit::computeFanoutFaninRatio(const LibParserCellInfo &cellinfo,
                                        double load) {
  return load / computeAvgInputCapacitance(cellinfo);
}  // end method

// -----------------------------------------------------------------------------

double Circuit::computeFanoutFaninRatio(Vcell *cell) {
  return computeFanoutFaninRatio(
      orgCells.oCells[cell->footprintIndex].cells[cell->actualInstTypeIndex],
      cell->actualLoad);
}  // end method

// -----------------------------------------------------------------------------

double Circuit::computeFanoutFaninRatioLE(Vcell *cell) {
  return computeFanoutFaninRatioLE(
      orgCells.oCells[cell->footprintIndex].cells[cell->actualInstTypeIndex],
      cell->actualLoad);
}  // end method

// -----------------------------------------------------------------------------

double Circuit::computeFanoutFaninRatioLEPosContest(Vcell *cell) {
  return computeFanoutFaninRatioLEPosContest(
      orgCells.oCells[cell->footprintIndex].cells[cell->actualInstTypeIndex],
      cell->actualLoad);
}  // end method

// -----------------------------------------------------------------------------

double Circuit::computeFanoutFaninRatioLE(const LibParserCellInfo &cellinfo,
                                          double load) {
  for (int j = 0; j < footprintLEPairs.size(); ++j) {
    if (cellinfo.footprint == footprintLEPairs[j].first)
      return load / (computeAvgInputCapacitance(cellinfo) *
                     footprintLEPairs[j].second);
  }  // end for
  assert(false);
  return 1;
}  // end method

// -----------------------------------------------------------------------------

double Circuit::computeFanoutFaninRatioLEPosContest(
    const LibParserCellInfo &cellinfo, double load) {
  for (int j = 0; j < footprintLEPairs.size(); ++j) {
    if (cellinfo.footprint == footprintLEPairs[j].first)
      return (load * footprintLEPairs[j].second) /
             computeInputCapacitance(cellinfo);
  }  // end for
  assert(false);
  return 1;
}  // end method

// -----------------------------------------------------------------------------

double Circuit::computeSlewViolation(const double slew,
                                     const int fanout) const {
  // [UPGRADE] Slew violation must be upgraded to reflect slew violation at
  //           net endpoints (sinks).

  return slew > maxTransition ? (fanout * (slew - maxTransition)) : 0;
}  // end method

// -----------------------------------------------------------------------------

void Circuit::sizingRandom() {
  for (int i = 0; i < icells.size(); i++) {
    Vcell *cell = icells[i];

    const int numCandidateCells =
        orgCells.oCells[cell->footprintIndex].cells.size();
    const int randomTypeIndex =
        floor((rand() / double(RAND_MAX)) * (numCandidateCells - 1) + 0.5);

    updateCellType(cell, randomTypeIndex);
  }  // end for

  updateTiming();
  // calcTiming();
}  // end method

// -----------------------------------------------------------------------------

void Circuit::sizingRandomFo4() {
  const int threshold = 0.1 * icells.size();

  int misses = 0;
  while (misses < threshold) {
    Vcell *cell = chooseCellRandomlyFromVector(icells);

    const int oldType = cell->actualInstTypeIndex;
    stepperFo4(cell);
    const int newType = cell->actualInstTypeIndex;

    if (oldType == newType) misses++;
  }  // end while

  // calcTiming();
  updateTiming();
}  // end method

// -----------------------------------------------------------------------------

void Circuit::sizingDepthFanout() {
  debug("flach", "Depth fanout sizing...\n");
  int iteration = 1;
  double previousWorstSlack = -numeric_limits<double>::max();
  do {
    previousWorstSlack = worstSlack;

    walkBackwardDepth(&Circuit::stepperFo4);
    // walkBackwardFromTemporalBarriers( &Circuit::stepperFo4 );

    // calcTiming();
    updateTiming();
    debug("flach", "\r\tIteration: " << iteration << "\tWorst Slack: "
                                     << worstSlack << "\t\t");

    iteration++;
  } while (previousWorstSlack != worstSlack && worstSlack < 0);
  debug("flach", "Depth fanout sizing... done\n");
}  // end method

// -----------------------------------------------------------------------------

void Circuit::sizingDepthFanoutX() {
  walkBackwardDepth(&Circuit::stepperFoX);
  // walkBackwardFromTemporalBarriers( &Circuit::stepperFo4 );

#ifdef COMPARE_TIMING_ENGINES
  // calcTiming();
  printTiming();
#endif
  updateTiming();
  printTiming();

}  // end method

// -----------------------------------------------------------------------------

void Circuit::sizingDepthFanoutXLogicalEffort() {
  debug("graci", "Depth fanout LE sizing...\n");
  int iterationA = 1;
  double previousWorstSlack = -numeric_limits<double>::max();
  // char nada;

  // calcTiming();
  assignLogicalEffort();

  do {
    previousWorstSlack = getWorstSlack();

    // calcTiming();
    walkBackwardDepth(&Circuit::stepperFoXLogicalEffort);

    updateTimingLR_KKT();

    // calcTiming();
    printTiming();

    debug("tiago2", "\n\tIteration: " << iterationA << "\tWorst Slack: "
                                      << getWorstSlack() << "\t\n");

    iterationA++;
  } while ((iterationA < 50) &&
           (getWorstSlack() < 0.0));  //( previousWorstSlack != getWorstSlack()
                                      //&& getWorstSlack() < 0);
  debug("graci", "Depth fanout LE sizing... done\n");

}  // end method

// -----------------------------------------------------------------------------

void Circuit::sizingSlewTarget() {
  debug("graci", "Assign slew targets...\n");
  cout << "Assign slew targets...\n" << endl;
  updateTiming();
  updateRequiredTime();

  int kMax = 10;

  double minSlewViol = this->slewViol;
  double minWorstSlack = this->worstSlack;

  // Set the minimum and maximum slew acceptable considering the lookups in
  // liberty file
  assignMinimumSlew();

  // Set the inicial slew target based in the max_transition definition from the
  // liberty file
  walkBackwardDepth(&Circuit::stepperSetSlewTarget);

  storeSolution();  // Store the initial solution to compare with the new
                    // solutions

  for (int i = 1; i <= kMax; i++) {
    double k = i;
    this->alpha = 1 / log(k + 1);
    // cout << "alpha = " << this->alpha << endl;

    walkBackwardDepth(&Circuit::stepperSetGlobalCriticality);
    walkBackwardDepth(
        &Circuit::stepperSetLocalCriticalityGC);  // step 1 of the Held's
                                                  // algorithm -- Update the
                                                  // slack targets
    walkBackwardDepth(&Circuit::stepperRefiningSlewTargets);
    walkBackwardDepth(
        &Circuit::stepperSetCellSlewTarget);  // step 2 of the Held's algorithm
                                              // -- Apply the slack targets
    walkForwardDepth(
        &Circuit::stepperRefineSlewTargets);  // step 3 of the Held's algorithm
                                              // -- Refine slew targets;

    // calcTiming();
    updateTiming();
    updateRequiredTime();
    printTiming();

    if (this->worstSlack >= minWorstSlack) {
      // storeBestSolution();
      storeSolution();
      minSlewViol = this->slewViol;
      minWorstSlack = this->worstSlack;
    }
  }  // end for

  cout << " Saving best solution found..." << endl;
  restoreFirstSolution();

  this->printTiming();

}  // end method

// -----------------------------------------------------------------------------

void Circuit::sizingGreedy() {
  cout << "Greedy sizing ...\n" << endl;

  walkBackwardDepth(
      &Circuit::stepperGreedySizing);  // Walk backward seting the best size for
                                       // each cell using local consideration

  // calcTiming();
  updateTiming();
  printTiming();

}  // end method

// -----------------------------------------------------------------------------

void Circuit::sizingByLiLi() {
  // Initial sizing for no load and slew violation.
  sizingForNoLoadAndSlewViolationByLiLi();
  updateTiming();
  printTimingDigest(true, "Li Li Sizing");

  // Sizing.
  const int numCells = depthSortedCells.size();

  double currentWorstSlack = getWorstSlack();
  double bestSlack = getWorstSlack();
  int iteration = 0;
  int oscillated = 0;
  int cont = 0;

  do {
    const int tag = iteration % 3;

    for (int k = offsetCombinational; k < numCells; k++) {
      Vcell *cell = depthSortedCells[k];

      if (cell->logicalDepth % 3 == tag) updateCellTypeByLiLi(cell);
    }  // end for

    updateTiming();
    updateLambdasByLiLi();

    printTimingDigest();

    const double worstSlack = getWorstSlack();
    const double delta = currentWorstSlack / worstSlack;

    // if ( currentWorstSlack >= 0 && loadViol <= 0 && 1 - delta < 0.02 && delta
    // < 1) {
    if (delta < 1.02) {
      oscillated++;
    } else {
      oscillated = 0;
    }  // end else

    currentWorstSlack = worstSlack;
    iteration++;

    if (currentWorstSlack >= 0 && loadViol <= 0) {
      cont++;
      if (currentWorstSlack > bestSlack) {
        storeSolution();
        bestSlack = currentWorstSlack;
      }  // end if
    }    // end if
    if (cont == 20) {
      restoreFirstSolution();
      break;
    }

  } while (oscillated < 10);

}  // end method

// -----------------------------------------------------------------------------

void Circuit::sizingLagrangeRelaxation(const double resetLambdas) {
  const int numArcs = timingArcs.size();
  const int numNets = timingNets.size();
  const int numCells = depthSortedCells.size();

  // Update timings.
  updateTiming();
  updateRequiredTime();
  printTimingDigest(true, "Lagrange Relaxation");

  storeSolution();

  const double T = sdcInfos.clk_period;
  alpha = 0.5 * T / timingWorstArrivalTime.getMax();

  if (resetLambdas) resetTimingArcLambdas(10);

  updateLambdasByFlach();
  // updateLambdasByTennakoon();

  double bestCost = +numeric_limits<double>::max();

  for (int iteration = 0; iteration < 100; iteration++) {
    int depth = 1;

    // Solve LSR.
    for (int k = offsetCombinational; k < numCells; k++) {
      Vcell *cell = depthSortedCells[k];
      updateCellTypeLagrangeRelaxation(cell /*, alpha*/);
    }  // end for

    // Update timings.
    updateTiming();
    updateRequiredTime();
    printTimingDigest();

    const double cost = getSlackSlack(getWorstSlack()) * totalLeakage;
    if (cost < bestCost && getSlackSlack(getWorstSlack()) < 1.1) {
      storeSolution();
      bestCost = cost;
    }  // end if

    // Update lambdas.
    updateLambdasByFlach();
    // updateLambdasByTennakoon();

    alpha *= T / timingWorstArrivalTime.getMax();
  }  // end for

  restoreFirstSolution();

}  // end method

// -----------------------------------------------------------------------------

void Circuit::sizingLagrangeRelaxationSensitivities(const double resetLambdas) {
  const bool report = true;
  const int reportNumDeltaVths = 5;    // -2 -1 0 +1 + 1
  const int reportNumDeltaSizes = 21;  // -10 -9 ... 0 ... +9 +10
  vector<int> reportSolution;
  vector<vector<int>> reportHistogram;

  if (report) {
    reportSolution.resize(icells.size());
    reportHistogram.resize(reportNumDeltaVths);
    for (int deltaVth = 0; deltaVth < reportNumDeltaVths; deltaVth++) {
      reportHistogram[deltaVth].resize(reportNumDeltaSizes, 0);
    }  // end for
  }    // end if

  const int numArcs = timingArcs.size();
  const int numNets = timingNets.size();
  const int numCells = depthSortedCells.size();

  //	FILE *pipe_gp = popen("gnuplot -persist", "w");
  //
  //	fputs("set term x11\n", pipe_gp);
  //	fputs("set grid\n", pipe_gp);
  //	fputs("set style data lines\n", pipe_gp );
  //	fputs("set logscale y\n", pipe_gp );
  //	fputs("set logscale y2\n", pipe_gp );

  vector<double> sampleTNS;
  vector<double> sampleLeakage;

  sampleTNS.reserve(101);
  sampleLeakage.reserve(101);

  // Update timings.
  updateTiming();
  updateRequiredTime();
  updateLambdaDelaySensitivities();

  printTimingDigest(true, "Lagrange Relaxation");

  sampleTNS.push_back(timingTotalNegativeSlack);
  sampleLeakage.push_back(totalLeakage);

  timingPreviousNetSlew.resize(numNets);
  for (int i = 0; i < numNets; i++)
    timingPreviousNetSlew[i] = getTimingNetState(i).slew;

  storeSolution();

  const double T = sdcInfos.clk_period;

  double alpha = 0.5 * T / timingWorstArrivalTime.getMax();
  double gamma = getSlackSlack(getWorstSlack());

  if (resetLambdas) resetTimingArcLambdas(12);

  updateLambdasByFlach();
  // updateLambdasByTennakoon();

  // saveHistogramPathSlacks(0);
  // saveHistogramLambdas(0);

  double bestCost = +numeric_limits<double>::max();

  for (int iteration = 0; iteration < 100; iteration++) {
    int depth = 1;

    if (report) {
      for (int i = 0; i < icells.size(); i++)
        reportSolution[i] = icells[i]->actualInstTypeIndex;
    }  // end else

    // Solve LSR.
    for (int k = offsetCombinational; k < numCells; k++) {
      Vcell *cell = depthSortedCells[k];
      updateCellTypeLagrangeRelaxationSensitivities(cell, gamma /*, alpha*/);
      // updateCellTypeLagrangeRelaxationSensitivitiesTestingChanges(cell, gamma
      // /*, alpha*/);
    }  // end for

    // Update timings.
    updateTiming();
    updateRequiredTime();
    updateLambdaDelaySensitivities();

    // cout <<setw(12) << (timingTotalPositiveSlack +
    // timingNumPathsWithNegativeSlack*timingTotalNegativeSlack);
    printTimingDigest();

    //		sampleTNS.push_back(timingTotalNegativeSlack);
    //		sampleLeakage.push_back(totalLeakage);
    //
    //		fputs("plot '-' u 1:2 axes x1y1 title \"TNS\", '-' u 1:3 axes
    // x1y2 title \"Leakage\"\n", pipe_gp); 		for (int i = 0; i <
    // sampleTNS.size(); ++i) 			fprintf(pipe_gp, "%d %f %f\n",
    // i, sampleTNS[i], sampleLeakage[i]); 		fputs("e\n", pipe_gp);
    // fflush(pipe_gp);

    for (int i = 0; i < numNets; i++)
      timingPreviousNetSlew[i] = getTimingNetState(i).slew;

    const double cost = getSlackSlack(getWorstSlack()) * totalLeakage;
    if (cost < bestCost && getSlackSlack(getWorstSlack()) < 1.1) {
      // const double cost = (timingTotalPositiveSlack +
      // timingNumPathsWithNegativeSlack*timingTotalNegativeSlack); if ( cost <
      // bestCost ) {
      storeSolution();
      bestCost = cost;
    }  // end if

    // Update lambdas.
    if (!optUseTennakoon) {
      updateLambdasByFlach();
    } else {
      updateLambdasByTennakoon();
    }  // end else

    alpha *= T / timingWorstArrivalTime.getMax();
    gamma = getSlackSlack(getWorstSlack());

    // saveHistogramPathSlacks(iteration+1);
    // saveHistogramLambdas(iteration+1);

    if (report && (iteration == 0 || iteration == 24 || iteration == 49 ||
                   iteration == 99)) {
      for (int deltaVth = 0; deltaVth < reportNumDeltaVths; deltaVth++) {
        reportHistogram[deltaVth].assign(reportHistogram[deltaVth].size(), 0);
      }  // end for

      for (int i = 0; i < icells.size(); i++) {
        if (icells[i]->dontTouch) continue;

        const LibCellCharacterization &char0 = getLibCellCharacterization(
            icells[i]->footprintIndex, reportSolution[i]);
        const LibCellCharacterization &char1 = getLibCellCharacterization(
            icells[i]->footprintIndex, icells[i]->actualInstTypeIndex);

        const int deltaSize = char1.size - char0.size;
        const int deltaVth = char1.vth - char0.vth;

        reportHistogram[deltaVth + 2][deltaSize + 10]++;
      }  // end method

      std::ostringstream oss;
      oss << "vth-size-change.report-" << (iteration + 1);

      ofstream file(oss.str().c_str());
      file << setw(8) << "-";
      for (int deltaVth = 0; deltaVth < reportNumDeltaVths; deltaVth++)
        file << setw(8) << (deltaVth - 2);
      file << "\n";
      for (int deltaSize = 0; deltaSize < reportNumDeltaSizes; deltaSize++) {
        file << setw(8) << (deltaSize - 10) << " ";
        for (int deltaVth = 0; deltaVth < reportNumDeltaVths; deltaVth++) {
          file << setw(8) << reportHistogram[deltaVth][deltaSize];
        }  // end for
        file << "\n";
      }  // end for
    }    // end else

  }  // end for

  restoreFirstSolution();
}  // end method

// -----------------------------------------------------------------------------

void Circuit::sizingLagrangeRelaxationTestingChanges(
    const double resetLambdas) {
  const int numArcs = timingArcs.size();
  const int numNets = timingNets.size();
  const int numCells = depthSortedCells.size();

  // Update timings.
  updateTiming();
  updateRequiredTime();
  printTimingDigest(true, "Lagrange Relaxation");

  storeSolution();

  const double T = sdcInfos.clk_period;
  alpha = 0.5 * T / timingWorstArrivalTime.getMax();

  if (resetLambdas) resetTimingArcLambdas(10);

  updateLambdasByFlach();
  // updateLambdasByTennakoon();

  double avg = 0;

  double bestCost = +numeric_limits<double>::max();

  for (int iteration = 0; iteration < 100; iteration++) {
    int depth = 1;

    // Solve LSR.
    for (int k = offsetCombinational; k < numCells; k++) {
      Vcell *cell = depthSortedCells[k];
      updateCellTypeLagrangeRelaxation(cell /*, alpha*/);
    }  // end for

    // Update timings.
    updateTiming();
    updateRequiredTime();
    printTimingDigest();

    const double cost = getSlackSlack(getWorstSlack()) * totalLeakage;

    const double oldavg = avg;

    avg = ((avg * iteration) + cost) / (iteration + 1);
    // cerr << setw(12) << avg/oldavg;

    if (cost < bestCost && getSlackSlack(getWorstSlack()) < 1.1) {
      storeSolution();
      bestCost = cost;
    }  // end if

    // Update lambdas.
    updateLambdasByFlach();
    // updateLambdasByTennakoon();

    alpha *= T / timingWorstArrivalTime.getMax();
  }  // end for

  restoreFirstSolution();

}  // end method

// -----------------------------------------------------------------------------

void Circuit::sizingLambdaGreedyStepSize() {
  const double T = getT();

  const int numArcs = timingArcs.size();
  const int numNets = timingNets.size();
  const int numCells = depthSortedCells.size();

  double UB;  // upper bound
  double LB;  // lower bound
  double L;   // current objective value (Lagrange dual problem result)
  double stepSize;

  // Compute initial bounds.
  UB = myRound(getTimingViolation(), 2) == 0 ? totalLeakage
                                             : +numeric_limits<double>::max();
  LB = 0;

  double totalMaxLeakage = 0;
  double totalMinLeakage = 0;
  for (int k = offsetCombinational; k < numCells; k++) {
    Vcell *cell = depthSortedCells[k];

    double maxLeakage = -numeric_limits<double>::max();
    double minLeakage = +numeric_limits<double>::max();

    const int numCandidateCells =
        orgCells.oCells[cell->footprintIndex].cells.size();
    for (int i = 0; i < numCandidateCells; i++) {
      const double leakage =
          orgCells.oCells[cell->footprintIndex].cells[i].leakagePower;

      maxLeakage = max(maxLeakage, leakage);
      minLeakage = min(minLeakage, leakage);
    }  // end for

    totalMaxLeakage += maxLeakage;
    totalMinLeakage += minLeakage;
  }  // end for

  UB = min(UB, totalMaxLeakage);
  LB = max(LB, totalMinLeakage);

  // Update timings.
  updateTiming();
  updateRequiredTime();
  printTimingDigest(true, "Lambda Greedy");

  storeSolution();

  // Initial Lagrange.
  resetTimingArcLambdas(0);
  L = computeLagrange();

  // Initial Lambdas
  stepSize = computeStepSize(UB, L);
  updateLambdasByTennakoonFig4(stepSize);

  double avg = 0;

  double bestCost = +numeric_limits<double>::max();

  for (int iteration = 0; iteration < 100; iteration++) {
    int depth = 1;

    // Solve LSR.
    for (int k = offsetCombinational; k < numCells; k++) {
      Vcell *cell = depthSortedCells[k];

      /*
       if (cell->logicalDepth > depth ) {
       // Update timings.
       updateTiming();
       updateRequiredTime();
       //printTimingDigest();
       depth = cell->logicalDepth;
       }
       */

      updateCellTypeLagrangeRelaxation(cell);
    }  // end for

    // Update timings.
    updateTiming();
    updateRequiredTime();

    cerr << setw(12) << LB << setw(12) << L << setw(12) << UB << setw(12)
         << stepSize;
    printTimingDigest();

    L = computeLagrange();
    if (myRound(getTimingViolation(), 2) == 0) UB = min(UB, totalLeakage);
    LB = max(LB, totalLeakage);

    const double cost = getSlackSlack(getWorstSlack()) * totalLeakage;
    if (cost < bestCost && getSlackSlack(getWorstSlack()) < 1.1) {
      storeSolution();
      bestCost = cost;
    }  // end if

    // Update lambdas.
    stepSize = computeStepSize(totalMaxLeakage, L);
    updateLambdasByTennakoonFig4(stepSize);
  }  // end for

  restoreFirstSolution();
}  // end method

// -----------------------------------------------------------------------------

void Circuit::sizingDepthFanoutXLogicalEffortPosContest() {
  debug("graci", "Depth fanout LE sizing...\n");
  int iteration = 1;
  double previousWorstSlack = -numeric_limits<double>::max();
  // char nada;

  // calcTiming();
  updateTiming();

  do {
    previousWorstSlack = getWorstSlack();

    walkBackwardDepth(&Circuit::stepperFoXLogicalEffortPosContest);

    updateTiming();
    printTiming();
    debug("graci", "\r\tIteration: " << iteration << "\tWorst Slack: "
                                     << getWorstSlack() << "\t\t");

    iteration++;
  } while (previousWorstSlack != getWorstSlack() && getWorstSlack() < 0);
  debug("graci", "Depth fanout LE sizing... done\n");

}  // end method

// -----------------------------------------------------------------------------

void Circuit::sizingDepthFanoutXLogicalEffortCopy() {
  int iteration = 1;
  double previousWorstSlack = -numeric_limits<double>::max();
  do {
    previousWorstSlack = getWorstSlack();

    walkBackwardDepth(&Circuit::stepperFoXLogicalEffortCopy);

    updateTiming();
    debug("flach", "\r\tIteration: " << iteration << "\tWorst Slack: "
                                     << getWorstSlack() << "\n");

    iteration++;
  } while (previousWorstSlack != getWorstSlack() && getWorstSlack() < 0);
  debug("graci", "Depth fanout LE sizing... done\n");
}  // end method

// -----------------------------------------------------------------------------

void Circuit::sizingDecreaseVthOfHighSpanCells() {
  for (int iteration = 0; iteration < 10; iteration++) {
    Vcell *cell = timingSortedCellsBySpan[iteration];
    const int n = cell->sinkNetIndex;

    if (decreaseVth(cell)) {
      updateTiming(cell);
      updateRequiredTime();
    }  // end if

    // cout <<setw(12) << timingNetSpan[n] << setw(12) << (originalInnerSlack -
    // bestCost) << "\n";
    printTimingDigest();
  }  // end for
}  // end method

// -----------------------------------------------------------------------------

void Circuit::sizingForNoLoadViolation() {
  if (loadViol > 0) {
    double previousLoadViolation;
    do {
      previousLoadViolation = loadViol;
      walkBackwardDepth(&Circuit::stepperNoLoadViolation);
    } while (previousLoadViolation > loadViol);
  }  // end if
}  // end method

// -----------------------------------------------------------------------------

void Circuit::sizingCriticalPathVTh() {
  const int numCells = icells.size();
  for (int i = 0; i < numCells; i++) {
    Vcell *cell = icells[i];
    if (cell->dontTouch) continue;

    updateCellType(cell, 0);
  }  // end for

  updateTiming();

  double previousWorstSlack =
      sdcInfos.clk_period - timingWorstArrivalTime.getMax();
  double bestWorstSlack = previousWorstSlack;

  int pathCounter = 1;

  for (int iteration = 0; iteration < 100000; iteration++) {
    updateTopCriticalPaths(pathCounter);

    Vcell *cell = chooseCellRandomlyFromVector(topCriticalCells);
    updateCellType(cell, min(29, cell->actualInstTypeIndex + 1));

    updateTiming(cell);

    const double currentWorstSlack =
        sdcInfos.clk_period - timingWorstArrivalTime.getMax();
    if (nearlyEqual(currentWorstSlack, previousWorstSlack)) pathCounter++;

    previousWorstSlack = currentWorstSlack;
    bestWorstSlack = max(bestWorstSlack, currentWorstSlack);

    cerr << "\rSlack = " << currentWorstSlack << "\t(" << bestWorstSlack
         << ")                         ";
  }  // end for
  cerr << "\n";

}  // end method

// -----------------------------------------------------------------------------

void Circuit::sizingCriticalPathSensitivity() {
  updateTiming();
  updateRequiredTime();

  if (timingTotalNegativeSlack <= 0) return;

  printTimingDigest(true, "Critical Path Sensitivity");

  const double originalLoadViolation = loadViol;

  for (int iteration = 0; iteration < 1000; iteration++) {
    updateCriticalPath();

    double bestSlack = getWorstSlack();
    double bestLeakage = +numeric_limits<double>::max();
    Vcell *bestCell = NULL;
    int bestCellType = -1;

    // Backward walking over critical path.
    const int numCells = criticalPath.size();
    for (int c = 0; c < numCells; c++) {
      Vcell *cell = criticalPath[c];

      const int originalTypeIndex = cell->actualInstTypeIndex;

      const int numCandidateCells =
          orgCells.oCells[cell->footprintIndex].cells.size();
      for (int i = 0; i < numCandidateCells; i++) {
        if (i == originalTypeIndex) continue;

        updateCellType(cell, i);
        updateTiming(cell);

        // Do not accept load violations.
        if (loadViol > originalLoadViolation) continue;

        bool accept;

        if (getWorstSlack() >= 0)
          accept = cell->getLeakagePower() < bestLeakage;
        else
          accept = getWorstSlack() > bestSlack;

        if (accept) {
          bestCell = cell;
          bestCellType = i;

          bestLeakage = cell->getLeakagePower();
          bestSlack = getWorstSlack();
        }  // end if
      }    // end for

      // Roll back to original cell type.
      updateCellType(cell, originalTypeIndex);
      updateTiming(cell);
    }  // end for

    if (bestCell) {
      updateCellType(bestCell, bestCellType);
      updateTiming(bestCell);
      printTimingDigest();

      if (getWorstSlack() >= 0) break;
    } else {
      cout << "No best cell :(\n";
      break;
    }  // end else

  }  // end for

  updateRequiredTime();

}  // end method

// -----------------------------------------------------------------------------

void Circuit::sizingProportionalDelay() {
  multimap<double, Vcell *> m;

  const int numNodes = icells.size();
  for (int i = 0; i < numNodes; i++) {
    Vcell *cell = icells[i];

    double delay = 0;

    const int k0 = timingArcPointers[cell->sinkNetIndex];
    const int k1 = timingArcPointers[cell->sinkNetIndex + 1];
    for (int k = k0; k < k1; k++) {
      delay = max(delay, getTimingArcState(k).delay.getMax());
    }  // end for

    const double targetDelay =
        sdcInfos.clk_period / double(cell->getPathDepth());

    m.insert(make_pair(targetDelay - delay, cell));
  }  // end for

  int counter = 1;
  for (multimap<double, Vcell *>::iterator it = m.begin(); it != m.end();
       it++) {
    Vcell *cell = it->second;

    if (it->first > 0 || counter > 100) break;

    // cerr << counter << "\t" << it->first << "\t" <<  cell->nextNum << "\t" <<
    // cell->instName << "\t" << cell->instType << "\n";

    updateCellType(cell, min(29, cell->actualInstTypeIndex + 1));

    counter++;
  }  // end for

  updateTiming();

}  // end method

// -----------------------------------------------------------------------------

void Circuit::timingRecovery() {
  updateTiming();
  updateRequiredTime();

  if (timingTotalNegativeSlack <= 0) return;

  printTimingDigest(true, "Timing Recovery");

  const double T = getT();
  const int numNets = timingNets.size();

  while (true) {
    int bestCell = -1;
    int bestChange = 0;
    double bestScore = 0;

    const double oldTNS = timingTotalNegativeSlack;

    updateCriticalPath();
    for (int i = 0; i < criticalPath.size(); i++) {
      Vcell *cell = criticalPath[i];

      if (cell->dontTouch) continue;

      const int originalTypeIndex = cell->actualInstTypeIndex;

      const double oldLeakage = cell->getLeakagePower();

      //			if ( decreaseVth(cell) ) {
      //				updateTiming(cell);
      //
      //				if ( timingTotalNegativeSlack <= oldTNS
      //) { 					const double score =
      //(oldTNS - timingTotalNegativeSlack) /
      //(cell->getLeakagePower() -
      // oldLeakage);
      //
      //					if ( score > bestScore ) {
      //						bestCell = i;
      //						bestChange = 0;
      //					} // end if
      //				} // end if
      //
      //				// Roll back.
      //				updateCellType( cell,
      // originalTypeIndex); 				updateTiming(cell);
      // } // end if

      if (upsize(cell)) {
        updateTiming(cell);

        if (timingTotalNegativeSlack <= oldTNS) {
          const double score = (oldTNS - timingTotalNegativeSlack) /
                               (cell->getLeakagePower() - oldLeakage);

          if (score > bestScore) {
            bestCell = i;
            bestChange = 1;
          }  // end if
        }    // end if

        // Roll back.
        updateCellType(cell, originalTypeIndex);
        updateTiming(cell);
      }  // end if

    }  // end for

    if (bestCell != -1) {
      Vcell *cell = criticalPath[bestCell];

      if (bestChange == 0)
        decreaseVth(cell);
      else
        upsize(cell);

      updateTiming(cell);
      updateRequiredTime();

      if (timingTotalNegativeSlack <= 0) break;
    } else {
      cout << "Timing recovery fail :( No best candidate cell.\n";
      break;
    }  // en

    printTimingDigest();
  }  // end while
}  // end method

// -----------------------------------------------------------------------------

void Circuit::timingRecoveryPathCounter() {
  updateTiming();
  updateRequiredTime();

  if (nearZero(timingTotalNegativeSlack)) return;

  updateCriticalPathCounter();

  printTimingDigest(true, "Timing Recovery - Path Counter");

  const int numNets = timingNetSortedByCriticalPathCounter.size();
  for (int i = 0; i < numNets; i++) {
    const int pathCounter = timingNetSortedByCriticalPathCounter[i].first;
    const int n = timingNetSortedByCriticalPathCounter[i].second;

    Vcell *cell = timingNets[n].driver;

    const int originalTypeIndex = cell->actualInstTypeIndex;
    const double oldTNS = timingTotalNegativeSlack;

    if (upsize(cell)) {
      updateTiming(cell);

      if (timingTotalNegativeSlack < oldTNS) {
        printTimingDigest();

        if (nearZero(timingTotalNegativeSlack)) break;

        updateCriticalPathCounter();
        i = 0;
      } else {
        // Roll back.
        updateCellType(cell, originalTypeIndex);
        updateTiming(cell);
      }  // end else
    }    // end if
  }      // end for

}  // end method

// -----------------------------------------------------------------------------

void Circuit::powerRecovery() {
  updateTiming();
  updateRequiredTime();

  printTimingDigest(true, "Power Recovery");

  const double T = getT();
  const int numNets = timingNets.size();

  int changedCounter = 0;
  int iteration = 0;

  do {
    changedCounter = 0;

    for (int n = timingNumDummyNets; n < numNets; n++) {
      // for ( int n = numNets-1; n >= timingNumDummyNets; n-- ) {
      const TimingNet &net = timingNets[n];

      Vcell *cell = timingNets[n].driver;
      if (!cell->dontTouch && !cell->actualInstType->isSequential) {
        bool changed = false;

        const int originalTypeIndex = cell->actualInstTypeIndex;
        const string originalTypeName = cell->instType;
        const double originalLeakagePower = cell->getLeakagePower();

        double currentTNS = timingTotalNegativeSlack;
        double currentWS = getWorstSlack();

        if (increaseVth(cell)) {
          updateTiming(cell);

          if (timingTotalNegativeSlack <= currentTNS) {
            changedCounter++;
            changed = true;
          } else {
            //						cout << "Cell#: "
            //							<< setw(7)  <<
            // cell->sinkNetIndex
            //							<< setw(7)  <<
            // originalTypeIndex
            //<<
            //"->"
            //<< cell->actualInstTypeIndex
            //							<< setw(20) <<
            // originalTypeName
            //<<
            //"->"
            //<< cell->instType
            //							<< setw(12) <<
            //(timingTotalNegativeSlack
            //- currentTNS)
            //							<< setw(10) <<
            // currentWS
            //<<
            //"->"
            //<< myRound(getWorstSlack(),2)
            //							<< setw(12) <<
            //(cell->actualLoad)
            //							<< setw(7) <<
            //(cell->nextNum)
            //							<< "\n";

            updateCellType(cell, originalTypeIndex);
            updateTiming(cell);
          }  // end else
        }    // end if
      }      // end if
    }        // end for

    for (int n = timingNumDummyNets; n < numNets; n++) {
      // for ( int n = numNets-1; n >= timingNumDummyNets; n-- ) {
      const TimingNet &net = timingNets[n];

      Vcell *cell = timingNets[n].driver;
      if (!cell->dontTouch && !cell->actualInstType->isSequential) {
        bool changed = false;

        const int originalTypeIndex = cell->actualInstTypeIndex;
        const string originalTypeName = cell->instType;
        const double originalLeakagePower = cell->getLeakagePower();

        double currentTNS = timingTotalNegativeSlack;
        double currentWS = getWorstSlack();

        if (!changed && downsize(cell)) {
          updateTiming(cell);

          if (timingTotalNegativeSlack <= currentTNS) {
            changedCounter++;
            changed = true;
          } else {
            //						cout << "Cell*: "
            //							<< setw(7)  <<
            // cell->sinkNetIndex
            //							<< setw(7)  <<
            // originalTypeIndex
            //<<
            //"->"
            //<< cell->actualInstTypeIndex
            //							<< setw(20) <<
            // originalTypeName
            //<<
            //"->"
            //<< cell->instType
            //							<< setw(12) <<
            //(timingTotalNegativeSlack
            //- currentTNS)
            //							<< setw(10) <<
            // currentWS
            //<<
            //"->"
            //<< myRound(getWorstSlack(),2)
            //							<< setw(12) <<
            //(cell->actualLoad)
            //							<< setw(7) <<
            //(cell->nextNum)
            //							<< "\n";

            updateCellType(cell, originalTypeIndex);
            updateTiming(cell);
          }  // end else
        }    // end if
      }      // end if
    }        // end for

    debug("flach", changedCounter << "\n");

    printTimingDigest();

    iteration++;

  } while (changedCounter > 0);
}  // end method

// -----------------------------------------------------------------------------

void Circuit::powerRecoveryByDecreasingVth() {
  updateTiming();
  updateRequiredTime();

  printTimingDigest(true, "Power Recovery by Decreasing Vth");

  const double T = getT();
  const int numNets = timingNets.size();

  int changedCounter = 0;
  int iteration = 0;

  do {
    changedCounter = 0;

    for (int n = timingNumDummyNets; n < numNets; n++) {
      // for ( int n = numNets-1; n >= timingNumDummyNets; n-- ) {
      const TimingNet &net = timingNets[n];

      Vcell *cell = timingNets[n].driver;
      if (cell->dontTouch) continue;

      bool changed = false;

      const int originalTypeIndex = cell->actualInstTypeIndex;
      const string originalTypeName = cell->instType;
      const double originalLeakagePower = cell->getLeakagePower();

      double currentTNS = timingTotalNegativeSlack;
      double currentWS = myRound(getWorstSlack(), 2);

      //
      LibCellGroupCharacterization &option =
          timingLibraryCharacterization[cell->footprintIndex];
      const int vth =
          option.cellCharacterization[cell->actualInstTypeIndex].vth;

      if (vth + 1 < option.mappingVthSizeToCellTypeIndex.size()) {
        // Find the lower size able to drive the cell load.
        int size0 = -1;
        int size1 = -1;
        for (int s = 0;
             s < option.mappingVthSizeToCellTypeIndex[vth + 1].size(); s++) {
          LibParserCellInfo &info =
              orgCells.oCells[cell->footprintIndex]
                  .cells[option.mappingVthSizeToCellTypeIndex[vth + 1][s]];

          if (info.pins[0].maxCapacitance >= cell->actualLoad) {
            if (size0 == -1) size0 = s;

            if (info.leakagePower > originalLeakagePower) {
              size1 = s;
              break;
            }
          }  // end if
        }    // end for

        if (size0 != -1 && size1 != -1) {
          // cout << "Net " << n << "\t" << size0 << ":" << size1 << "\n";

          for (int size = size0; size < size1; size++) {
            updateCellType(cell,
                           option.mappingVthSizeToCellTypeIndex[vth + 1][size]);
            updateTiming(cell);

            if (timingTotalNegativeSlack <= currentTNS) {
              changedCounter++;
            } else {
              //							cout <<
              //"Cell$: "
              //								<<
              // setw(7)  << cell->sinkNetIndex
              //								<<
              // setw(7)  << originalTypeIndex << "->" <<
              // cell->actualInstTypeIndex
              //								<<
              // setw(20) << originalTypeName << "->" <<  cell->instType
              //								<<
              // setw(12) << (timingTotalNegativeSlack - currentTNS)
              //								<<
              // setw(10) << currentWS << "->" << myRound(getWorstSlack(),2)
              //								<<
              // setw(12) << (cell->getLeakagePower() - originalLeakagePower )
              //								<<
              // setw(12) << (cell->actualLoad)
              //								<<
              //"\n";

              // Rollback
              updateCellType(cell, originalTypeIndex);
              updateTiming(cell);
            }  // end else

          }  // end for
        } else {
          cout << ".";
        }  // end else
      }    // end if

    }  // end for

    updateRequiredTime();

    debug("flach", changedCounter << "\n");

    printTimingDigest();

    iteration++;

  } while (changedCounter > 0);
}  // end method

// -----------------------------------------------------------------------------

double Circuit::lookup(const LibParserLUT &lut, const double x,
                       const double y) {
  double weightX, weightY;
  double xLower, xUpper, yLower, yUpper;
  int xLowerIndex, xUpperIndex, yLowerIndex, yUpperIndex, xLimit, yLimit;

  xLowerIndex = xUpperIndex = yLowerIndex = yUpperIndex = 0;
  xLimit = lut.loadIndices.size() - 2;
  yLimit = lut.transitionIndices.size() - 2;

  // no loads viol. are accepted -> not anymore
  while ((xLowerIndex < xLimit) && (lut.loadIndices[xLowerIndex + 1] <= x))
    ++xLowerIndex;
  xUpperIndex = xLowerIndex + 1;

  while ((yLowerIndex < yLimit) &&
         (lut.transitionIndices[yLowerIndex + 1] <= y))
    ++yLowerIndex;
  yUpperIndex = yLowerIndex + 1;

  xLower = lut.loadIndices[xLowerIndex];
  xUpper = lut.loadIndices[xUpperIndex];
  yLower = lut.transitionIndices[yLowerIndex];
  yUpper = lut.transitionIndices[yUpperIndex];

  weightX = (x - xLower) / (xUpper - xLower);
  weightY = (y - yLower) / (yUpper - yLower);

  double result;
  result = (1.0 - weightX) * (1.0 - weightY) *
           (lut.tableVals[xLowerIndex][yLowerIndex]);
  result +=
      (weightX) * (1.0 - weightY) * (lut.tableVals[xUpperIndex][yLowerIndex]);
  result +=
      (1.0 - weightX) * (weightY) * (lut.tableVals[xLowerIndex][yUpperIndex]);
  result += (weightX) * (weightY) * (lut.tableVals[xUpperIndex][yUpperIndex]);

  return result;
}  // end method

// -----------------------------------------------------------------------------

double Circuit::lookupDelay(const LibParserTimingInfo &timingInfo,
                            const EdgeType edgeType, const double inputSlew,
                            const double loadCapacitance) {
  const LibParserLUT &lut =
      edgeType == FALL ? timingInfo.fallDelay : timingInfo.riseDelay;
  const double x = loadCapacitance;
  const double y = inputSlew;

  return lookup(lut, x, y);
}  // end method

// -----------------------------------------------------------------------------

double Circuit::lookupOutputSlew(const LibParserTimingInfo &timingInfo,
                                 const EdgeType edgeType,
                                 const double inputSlew,
                                 const double loadCapacitance) {
  const LibParserLUT &lut =
      edgeType == FALL ? timingInfo.fallTransition : timingInfo.riseTransition;
  const double x = loadCapacitance;
  const double y = inputSlew;

  return lookup(lut, x, y);
}  // end method

// -----------------------------------------------------------------------------

void Circuit::computeArcTiming(const LibParserTimingInfo &timingInfo,
                               const EdgeArray<double> inputSlew,
                               const double load, EdgeArray<double> &outDelay,
                               EdgeArray<double> &outSlew) {
  const double xOutputLoad = load;
  const double yInputRiseSlew = inputSlew[RISE];
  const double yInputFallSlew = inputSlew[FALL];

  // Delay
  outDelay[RISE] =
      Circuit::lookup(timingInfo.riseDelay, xOutputLoad, yInputFallSlew);
  outDelay[FALL] =
      Circuit::lookup(timingInfo.fallDelay, xOutputLoad, yInputRiseSlew);

  // Output Slew
  outSlew[RISE] =
      Circuit::lookup(timingInfo.riseTransition, xOutputLoad, yInputFallSlew);
  outSlew[FALL] =
      Circuit::lookup(timingInfo.fallTransition, xOutputLoad, yInputRiseSlew);
}  // end method

// -----------------------------------------------------------------------------

void Circuit::computeArcTimingDelayAndSlewSensitivityToOutputLoad(
    const int k, const int size, EdgeArray<double> &sensitivityDelay,
    EdgeArray<double> &sensitivitySlew) {
  const TimingArc &arc = timingArcs[k];
  const TimingNet &net = timingNets[arc.sink];

  const TimingArcState &arcstate = getTimingArcState(k);
  const TimingNetState &netstate = getTimingNetState(arc.sink);

  const LibParserCellInfo &cellinfo =
      orgCells.oCells[arc.cell->footprintIndex].cells[size];
  const LibParserTimingInfo &timingInfo = cellinfo.timingArcs[arc.lut];

  const double outputLoad0 = netstate.load - sensitivityOffsetOutputLoad;
  const double outputLoad1 = netstate.load + sensitivityOffsetOutputLoad;

  EdgeArray<double> delay0;
  EdgeArray<double> delay1;

  EdgeArray<double> slew0;
  EdgeArray<double> slew1;

  computeArcTiming(timingInfo, arcstate.islew, outputLoad0, delay0, slew0);
  computeArcTiming(timingInfo, arcstate.islew, outputLoad1, delay1, slew1);

  const double deltaLoad = 2 * sensitivityOffsetOutputLoad;
  sensitivityDelay = (delay1 - delay0) / (deltaLoad);
  sensitivitySlew = (slew1 - slew0) / (deltaLoad);
}  // end method

// -----------------------------------------------------------------------------

void Circuit::computeArcTimingDelayAndSlewSensitivityToInputSlew(
    const int k, const int size, EdgeArray<double> &sensitivityDelay,
    EdgeArray<double> &sensitivitySlew) {
  const TimingArc &arc = timingArcs[k];
  const TimingNet &net = timingNets[arc.sink];

  const TimingArcState &arcstate = getTimingArcState(k);
  const TimingNetState &netstate = getTimingNetState(arc.sink);

  const LibParserCellInfo &cellinfo =
      orgCells.oCells[arc.cell->footprintIndex].cells[size];
  const LibParserTimingInfo &timingInfo = cellinfo.timingArcs[arc.lut];

  const EdgeArray<double> inputSlew0 =
      arcstate.islew - sensitivityOffsetInputSlew;
  const EdgeArray<double> inputSlew1 =
      arcstate.islew + sensitivityOffsetInputSlew;

  EdgeArray<double> delay0;
  EdgeArray<double> delay1;

  EdgeArray<double> slew0;
  EdgeArray<double> slew1;

  computeArcTiming(timingInfo, inputSlew0, netstate.load, delay0, slew0);
  computeArcTiming(timingInfo, inputSlew1, netstate.load, delay1, slew1);

  const double deltaSlew = 2 * sensitivityOffsetInputSlew;
  sensitivityDelay = (delay1 - delay0) / (deltaSlew);
  sensitivitySlew = (slew1 - slew0) / (deltaSlew);
}  // end method

// -----------------------------------------------------------------------------

void Circuit::createCloadVector() {
  const int numCells = icells.size();
  cloadOrdered.resize(numCells);

  for (int i = 0; i < numCells; i++) cloadOrdered[i] = icells[i];
}  // end method

// -----------------------------------------------------------------------------

void Circuit::cloadOrder() {
  const int numCells = cloadOrdered.size();
  Vcell *aux;

  for (int i = 0; i < numCells; i++) {
    for (int j = 0; j < numCells - 1; j++) {
      if (cloadOrdered[j]->actualLoad < cloadOrdered[j + 1]->actualLoad) {
        aux = cloadOrdered[j];
        cloadOrdered[j] = cloadOrdered[j + 1];
        cloadOrdered[j + 1] = aux;
      }  // end if
    }    // end for

    // storedSolution[i] = icells[i]->actualInstTypeIndex;
  }  // end for
}  // end method

// -----------------------------------------------------------------------------

void Circuit::colorTemperature(const double weight, int &r, int &g,
                               int &b) const {
  // Mangeta <-> Blue <-> Cian <-> Green <-> Yellow <-> Red

  const int numColors = 6;
  static const int R[numColors] = {255, 0, 0, 0, 255, 255};
  static const int G[numColors] = {0, 0, 255, 255, 255, 0};
  static const int B[numColors] = {255, 255, 255, 0, 0, 0};

  double temp;
  if (weight > 1.0)
    temp = (numColors - 1);
  else if (weight < 0)
    temp = 0;
  else
    temp = (numColors - 1) * weight;

  const int i0 = (int)floor(temp);
  const int i1 = (int)ceil(temp);
  const double alpha = temp - (int)(temp);

  r = (int)floor(R[i0] * (1 - alpha) + R[i1] * (alpha) + 0.5);
  g = (int)floor(G[i0] * (1 - alpha) + G[i1] * (alpha) + 0.5);
  b = (int)floor(B[i0] * (1 - alpha) + B[i1] * (alpha) + 0.5);
}  // end method

// -----------------------------------------------------------------------------

void Circuit::storeBestSolution() { bestSolution = copySizes(); }  // end method

// -----------------------------------------------------------------------------

void Circuit::saveBestSizes() {
  // save best solution to .sizes file

  FILE *pfile;
  string filename = rootDir + "/" + benchName + "/" + benchName + ".sizes";

  if (pfile = fopen(filename.c_str(), "w")) {
    cout << " Saving sizes..." << endl;
    set<Vcell *>::iterator it;
    // Vcell* tmpCell;

    for (int i = 0; i < bestSolution.size(); ++i) {
      fprintf(pfile, "%s %s\n", bestSolution[i].first.c_str(),
              bestSolution[i].second.c_str());
    }
    fclose(pfile);
    cout << " Saving sizes...done" << endl;
  } else
    cerr << "Problem saving .sizes file!" << endl;
}  // end method

// -----------------------------------------------------------------------------

void Circuit::storeSolution() {
  const int numCells = icells.size();

  storedSolution.resize(numCells);
  for (int i = 0; i < numCells; i++)
    storedSolution[i] = icells[i]->actualInstTypeIndex;
}  // end method

// -----------------------------------------------------------------------------

void Circuit::storePastSolution() {
  const int numCells = icells.size();

  storedPastSolution.resize(numCells);
  for (int i = 0; i < numCells; i++) storedPastSolution[i] = storedSolution[i];
}  // end method

// -----------------------------------------------------------------------------

void Circuit::restoreFirstSolution() {
  const int numCells = icells.size();
  for (int i = 0; i < numCells; i++)
    if (!icells[i]->dontTouch) updateCellType(icells[i], storedSolution[i]);
  // calcTiming();
  updateTiming();
}  // end method

// -----------------------------------------------------------------------------

void Circuit::restoreSolution() {
  const int numCells = icells.size();
  for (int i = 0; i < numCells; i++)
    if (!icells[i]->dontTouch) updateCellType(icells[i], storedPastSolution[i]);
  // calcTiming();
  updateTiming();
}  // end method

// -----------------------------------------------------------------------------

void Circuit::buildTimingStructure() {
  int counterArcs_InputDrivers = 0;
  int counterArcs_Sequentials = 0;
  int counterArcs_Combinationals = 0;

  // Map used to sort cells by their depth.
  // Key: cell index
  // Value: cell pointer
  multimap<int, Vcell *> m;

  for (int i = 0; i < graphRoot.nextCells.size(); i++)
    m.insert(make_pair(0, graphRoot.nextCells[i]));
  counterArcs_InputDrivers += graphRoot.nextCells.size();

  const int numNodes = icells.size();
  for (int i = 0; i < numNodes; i++) {
    Vcell *cell = icells[i];
    m.insert(make_pair(cell->logicalDepth, cell));

    if (cell->actualInstType->isSequential) {
      counterArcs_Sequentials += 1;  // only accounts for clk to out arc
    } else {
      counterArcs_Combinationals += cell->actualInstType->timingArcs.size();
    }  // end else
  }    // end for

  const int counterDummyArcs =
      counterArcs_InputDrivers + counterArcs_Sequentials;

  const int counterExtraAcrs = counterArcs_Sequentials + outputs.size();

  const int counterArcs =
      counterDummyArcs + counterArcs_Combinationals + counterExtraAcrs;

  timingOffsetToCombinationArcs = counterDummyArcs;
  timingOffsetToExtraSequentialArcs =
      counterDummyArcs + counterArcs_Combinationals;
  timingOffsetToExtraPrimaryOutputArcs =
      timingOffsetToExtraSequentialArcs + counterArcs_Sequentials;

  // Dummy Nets: Timing arcs of input drivers and sequential cells are
  // driven by dummy nets. Each input driver has a unique dummy net and all
  // sequential cells have a same dummy net (clock). Dummy nets allow us to
  // deal with input drivers and sequential cells exactly in the same way as
  // combinational cells.
  const int dummyNets = counterArcs_InputDrivers + 1;  // +1 for clock net

  // Offsets
  timingNumDummyNets = dummyNets;

  // Resize vectors appropriately.
  timingNets.resize(dummyNets + (nets.size() - 1));  // -1 discount clock net
  timingNetLambdaDelaySensitivity.resize(timingNets.size());

  timingArcs.resize(counterArcs);
  timingArcLambdaDelaySensitivity.resize(timingArcs.size());

  timingArcPointers.resize(timingNets.size() + 1);
  timingArcPointers.back() = timingOffsetToExtraSequentialArcs;

  timingNetName.resize(timingNets.size());
  timingMaxLoad.resize(timingNets.size());
  timingRequiredTime.resize(timingNets.size());

  timingArcDriverPinName.resize(timingArcs.size());
  timingArcSinkPinName.resize(timingArcs.size());

  timingStateCurrent.arcs.resize(timingArcs.size());
  timingStateCurrent.nets.resize(timingNets.size());

  // Setup timing of dummy nets of input drivers.
  assert(dummyNets - 1 == graphRoot.nextCells.size());

  for (int i = 0; i < dummyNets - 1; i++) {
    TimingNetState &netstate = getTimingNetState(i);

    const Vcell *cell = graphRoot.nextCells[i];
    const LibParserTimingInfo &timingInfo = cell->actualInstType->timingArcs[0];

    netstate.slew[RISE] = cell->inputSlews[0].first;
    netstate.slew[FALL] = cell->inputSlews[0].second;

    netstate.arrivalTime[RISE] = sdcInfos.inputDelays[i].delay;
    netstate.arrivalTime[FALL] = sdcInfos.inputDelays[i].delay;

    netstate.arrivalTime[RISE] -=
        Circuit::lookup(timingInfo.riseDelay, 0, netstate.slew[RISE]);
    netstate.arrivalTime[FALL] -=
        Circuit::lookup(timingInfo.fallDelay, 0, netstate.slew[FALL]);
  }  // end for

  // Setup timing of the dummy net of sequantial cells.
  {
    TimingNetState &netstate = getTimingNetState(dummyNets - 1);
    netstate.slew =
        EdgeArray<double>(0, 0);  // [ISPD CONTEST] Output slew of flip-flops
                                  // does not depends on input slew.
    netstate.arrivalTime = EdgeArray<double>(0, 0);
  }  // end block

  // Now we sweep cells sorted by their logical depth (and kind). First
  // we got input drivers, next sequential cells (flip-flops) and then
  // combinational cells.

  // Map used to map netnames to netindex in timing structure.
  map<string, int> netMap;

  int indexNet = timingNumDummyNets;
  int indexArc = 0;

  int counterDummyNets = 0;

  for (multimap<int, Vcell *>::iterator it = m.begin(); it != m.end(); it++) {
    Vcell *cell = it->second;

    // Create a net index for each output pin.
    const vector<LibParserPinInfo> &pins = cell->actualInstType->pins;
    for (int i = 0; i < pins.size(); i++) {
      const LibParserPinInfo &pininfo = pins[i];
      if (!pininfo.isInput) {
        string sinkNet = cell->returnNetConnectedToPin(pininfo.name);
        assert(sinkNet != "");

        netMap[sinkNet] = indexNet;
        timingNetName[indexNet] = sinkNet;
        timingArcPointers[indexNet] = indexArc;

        TimingNet &net = timingNets[indexNet];
        net.depth = cell->logicalDepth;
        net.driver = cell;

        TimingNetState &netstate = getTimingNetState(indexNet);
        netstate.load = cell->actualLoad;

        timingMaxLoad[indexNet] = pininfo.maxCapacitance;

        const vector<LibParserTimingInfo> &arcs =
            cell->actualInstType->timingArcs;
        for (int k = 0; k < arcs.size(); k++) {
          if (pininfo.name == arcs[k].toPin) {
            TimingArc &arc = timingArcs[indexArc];
            arc.cell = cell;
            arc.lut = k;
            arc.pin = cell->returnPinIndex(arcs[k].fromPin);
            arc.sink = indexNet;

            timingArcDriverPinName[indexArc] = arcs[k].fromPin;
            timingArcSinkPinName[indexArc] = arcs[k].toPin;

            if (cell->logicalDepth == 0) {
              if (counterDummyNets >= dummyNets - 1) {
                arc.driver = dummyNets - 1;  // clock dummy net index
                assert(cell->actualInstType->isSequential);
              } else {
                arc.driver = counterDummyNets;
                assert(cell->instName == "inputDriver");
              }  // end else

              counterDummyNets++;
            } else {
              arc.driver =
                  netMap[cell->returnNetConnectedToPin(arcs[k].fromPin)];
              assert(!cell->actualInstType->isSequential);
              assert(cell->instName != "inputDriver");
            }  // end else

            indexArc++;

            assert(indexArc <= timingOffsetToExtraSequentialArcs);

          }  // end if
        }    // end for

        // [WARNING][TODO] The following statement is assuming a cell has only
        // one output.
        cell->sinkNetIndex = indexNet;

        indexNet++;
      }  // end if
    }    // end for
  }      // end for

  assert(indexArc == timingOffsetToExtraSequentialArcs);

  // Sweep sequential elements and setup extra timing arcs
  for (int i = offsetSequential; i < offsetCombinational; i++) {
    Vcell *cell = depthSortedCells[i];

    assert(cell->actualInstType->isSequential);

    int counter = 0;
    const vector<LibParserPinInfo> &pins = cell->actualInstType->pins;
    for (int i = 0; i < pins.size(); i++) {
      const LibParserPinInfo &pininfo = pins[i];
      if (pininfo.isInput && !pininfo.isClock) {
        map<string, int>::iterator it =
            netMap.find(cell->returnNetConnectedToPin(pininfo.name));
        assert(it != netMap.end());

        TimingArc &arc = timingArcs[indexArc];
        arc.driver = it->second;
        arc.sink = -1;
        arc.lut = -1;
        arc.pin = i;
        arc.cell = cell;

        TimingArcState &arcstate = getTimingArcState(indexArc);
        arcstate.delay.set(0, 0);

        timingArcDriverPinName[indexArc] = pininfo.name;

        indexArc++;
        counter++;
      }  // end if
    }    // end for

    assert(counter == 1);
  }  // end for

  assert(indexArc == timingOffsetToExtraPrimaryOutputArcs);

  // Sweep output nets and setup extra timing arcs.
  for (int i = 0; i < outputs.size(); i++) {
    const pair<string, Vcell *> p = outputs[i];
    const string netName = p.first;
    Vcell *netDriver = p.second;

    map<string, int>::iterator it = netMap.find(netName);
    assert(it != netMap.end());

    const int netIndex = it->second;

    TimingArc &arc = timingArcs[indexArc++];
    arc.driver = netIndex;
    arc.sink = -1;
    arc.lut = -1;
    arc.pin = -1;
    arc.cell = NULL;

    assert(netDriver == timingNets[arc.driver].driver);
  }  // end for

  assert(indexArc == counterArcs);

  // Compute net fanouts.
  for (int i = 0; i < timingArcs.size(); i++) {
    const TimingArc &arc = timingArcs[i];
    timingNets[arc.driver].fanout++;
  }  // end for

  // Build sink list for each net.
  vector<vector<int>> netsinks(timingNets.size());
  for (int i = 0; i < timingArcs.size(); i++) {
    const TimingArc &arc = timingArcs[i];
    netsinks[arc.driver].push_back(i);
  }  // end for

  // Sink nets and sink arcs.
  timingSinkNets.resize(timingArcs.size() + 1);
  timingSinkNetPointers.resize(timingNets.size() + 1);

  timingSinkArcs.resize(timingArcs.size() + 1);
  timingSinkArcPointers.resize(timingNets.size() + 1);
  timingSinkArcPointers.back() = timingArcs.size();

  int sinkNetPointer = 0;
  int sinkArcPointer = 0;
  for (int i = 0; i < timingNets.size(); i++) {
    timingSinkArcPointers[i] = sinkArcPointer;
    timingSinkNetPointers[i] = sinkNetPointer;

    const int numSinks = netsinks[i].size();
    for (int k = 0; k < numSinks; k++) {
      const int arcIndex = netsinks[i][k];

      if (timingArcs[arcIndex].sink != -1) {
        bool duplicate = false;
        // Avoid duplicates which may happen in some circuit designs
        // when a net drives multiple inputs of a same cell (e.g DMA
        // ISPD 2012 benchmark).
        for (int k = timingSinkNetPointers[i]; k < sinkNetPointer; k++) {
          if (timingSinkNets[k] == timingArcs[arcIndex].sink) {
            duplicate = true;
            break;
          }  // end if
        }    // end for

        if (!duplicate)
          timingSinkNets[sinkNetPointer++] = timingArcs[arcIndex].sink;
      }  // end if

      timingSinkArcs[sinkArcPointer++] = arcIndex;
    }  // end for
  }    // end for

  timingSinkNetPointers.back() = sinkNetPointer;

  assert(timingSinkArcPointers[timingNets.size() - 1] <=
         timingSinkArcPointers[timingNets.size() - 1]);

  // Driver nets.
  timingDriverNets.reserve(timingArcs.size() + 1);
  timingDriverNetPointers.resize(timingNets.size() + 1);

  for (int i = 0; i < timingNets.size(); i++) {
    set<int> drivers;  // using a set to eliminate duplicates.

    const int k0 = timingArcPointers[i];
    const int k1 = timingArcPointers[i + 1];
    for (int k = k0; k < k1; k++) {
      drivers.insert(timingArcs[k].driver);
    }  // end for

    timingDriverNetPointers[i] = timingDriverNets.size();
    for (set<int>::const_iterator it = drivers.begin(); it != drivers.end();
         it++)
      timingDriverNets.push_back(*it);
  }  // end for

  timingDriverNetPointers.back() = timingDriverNets.size();

  // Setup input slew and arrival time for arcs belonging to dummy nets.
  for (int i = 0; i < dummyNets; i++) {
    const TimingNetState &netstate = getTimingNetState(i);

    const int k0 = timingSinkArcPointers[i];
    const int k1 = timingSinkArcPointers[i + 1];
    for (int k = k0; k < k1; k++) {
      // Update arc timings.
      TimingArcState &arcstate = getTimingArcState(timingSinkArcs[k]);

      arcstate.arrivalTime = netstate.arrivalTime;
      arcstate.islew = netstate.slew;
    }  // end for
  }    // end for

  // Find out tail nets -- the ones driving a sequential element or a primary
  // output.
  multiset<string> tailNets;

  for (int i = 0; i < numNodes; i++) {
    Vcell *cell = icells[i];

    if (cell->actualInstType->isSequential) {
      const vector<LibParserPinInfo> &pins = cell->actualInstType->pins;
      for (int i = 0; i < pins.size(); i++) {
        const LibParserPinInfo &pininfo = pins[i];
        if (pininfo.isInput && !pininfo.isClock) {
          string netname = cell->returnNetConnectedToPin(pininfo.name);
          tailNets.insert(netname);
        }  // end if
      }    // end for
    }      // end if
  }        // end for

  for (int i = 0; i < outputs.size(); i++) tailNets.insert(outputs[i].first);

  timingTailNets.reserve(tailNets.size());
  timingTailNetMultiplicities.reserve(tailNets.size());

  string previousNetname = "";
  for (multiset<string>::iterator it = tailNets.begin(); it != tailNets.end();
       it++) {
    const string netname = *it;

    if (previousNetname != netname) {
      map<string, int>::iterator itm = netMap.find(netname);
      assert(itm != netMap.end());

      previousNetname = netname;

      timingTailNets.push_back(itm->second);
      timingTailNetMultiplicities.push_back(1);
    } else {
      timingTailNetMultiplicities.back()++;
    }  // end else
  }    // end for

  // Setup initial lambda values;
  for (int i = 0; i < timingArcs.size(); i++) {
    getTimingArcState(i).lambda = EdgeArray<double>(1, 1);
  }

  // Find offset to level nets.
  timingOffsetToNetLevel.resize((maxLogicalDepth + 1) + 1,
                                -1);  // +1 to store a dummy offset
  timingOffsetToNetLevel.front() = timingNumDummyNets;
  timingOffsetToNetLevel.back() = timingNets.size();

  int depth = 0;
  for (int i = timingNumDummyNets; i < timingNets.size(); i++) {
    if (timingNets[i].depth > depth) timingOffsetToNetLevel[++depth] = i;
  }  // end for

  timingOffsetToLevelOneNets = timingOffsetToNetLevel[1];

#ifndef NDEBUG
  if (indexNet != timingNets.size()) {
    cerr << "[BUG] @ Circuit::buildTimingStructure() - "
         << "Divergent number of nets. Expected " << (timingNets.size())
         << " got " << indexNet << ".\n";
  }  // end if

  if (indexArc != timingArcs.size()) {
    cerr << "[BUG] @ Circuit::buildTimingStructure() - "
         << "Divergent number of arcs. Expected " << timingArcs.size()
         << " got " << indexArc << ".\n";
  }  // end if

  // Check if all timingNets have been correctly initialized.
  for (int i = timingNumDummyNets; i < timingNets.size(); i++) {
    const TimingNet &net = timingNets[i];

    if (!net.driver)
      cerr << "[BUG] @ Circuit::buildTimingStructure() - Net " << i
           << " has no driver.\n";

    if (net.depth == -1)
      cerr << "[BUG] @ Circuit::buildTimingStructure() - Net " << i
           << " has invalid depth.\n";
  }  // end for

  // Check if tail nets match with nets driven by tail cells.
  for (int i = 0; i < pathTails.size(); i++) {
    const int n = pathTails[i]->sinkNetIndex;

    bool found = false;
    for (int k = 0; k < timingTailNets.size(); k++) {
      if (n == timingTailNets[k]) {
        found = true;
        break;
      }  // end if
    }    // end for

    if (!found)
      cerr << "[BUG] @ Circuit::buildTimingStructure() - Net " << n
           << " should be in tail net list.\n";
  }  // end for

  // Check if tail nets are driving by a tail cell.
  for (int i = 0; i < timingTailNets.size(); i++) {
    const Vcell *driver = timingNets[timingTailNets[i]].driver;

    // cerr << "\tNet " << i << "\t" << driver->instName << " (" <<
    // driver->instType << ")\n";

    bool found = false;
    for (int k = 0; k < pathTails.size(); k++) {
      if (driver == pathTails[k]) {
        found = true;
        break;
      }  // end if
    }    // end for

    if (!found)
      cerr << "[BUG] @ Circuit::buildTimingStructure() - Cell "
           << driver->instName << " (" << driver->instType
           << ") should be in tail net list.\n";
  }  // end for

  // Check if fanout matches the nextNum property in Vcell.
  for (int i = timingNumDummyNets; i < timingNets.size(); i++) {
    const TimingNet &net = timingNets[i];

    if (net.fanout != net.driver->nextNum) {
      cerr << "[BUG] @ Circuit::buildTimingStructure() - Divergent fanout "
              "numbers for net "
           << timingNetName[i] << " and its driver.\n"
           << "\tNet Fanout.........: " << net.fanout << "\t"
           << timingNetName[i] << "\n"
           << "\tNet's Driver Fanout: " << net.driver->nextNum << "("
           << net.driver->nextCells.size() << ") "
           << "\t" << net.driver->instName << " (" << net.driver->instType
           << ")"
           << "\n";
    }  // end if
  }    // end for

#endif

}  // end method

// -----------------------------------------------------------------------------

#ifdef PARALLEL

void Circuit::setupMultithreading(const int numThreads) {
  threadNumThreads =
      numThreads == 0 ? Poco::Environment::processorCount() : numThreads;

  cout << "Multithreading: Setting up the thread pool to " << threadNumThreads
       << " threads.\n";
  myThreadPool.addCapacity(max(0, threadNumThreads - myThreadPool.capacity()));

  timingViolationSlewVector.resize(threadNumThreads, 0);

  threadNetPointers.resize(maxLogicalDepth + 1);
  for (int i = 0; i < threadNetPointers.size(); i++)
    threadNetPointers[i].resize(threadNumThreads + 1);

  const int minNumNetsPerThread = 1;

  for (int depth = 0; depth <= maxLogicalDepth; depth++) {
    const int k0 = timingOffsetToNetLevel[depth];
    const int k1 = timingOffsetToNetLevel[depth + 1];

    const int numNetsAtThisLevel = k1 - k0;
    const int numNetsPerThread = numNetsAtThisLevel / threadNumThreads;

    int offset = timingOffsetToNetLevel[depth];
    for (int t = 0; t < threadNumThreads - 1; t++) {
      threadNetPointers[depth][t] = offset;
      offset += numNetsPerThread;
    }  // end for

    threadNetPointers[depth][threadNumThreads - 1] = offset;
    threadNetPointers[depth][threadNumThreads] =
        timingOffsetToNetLevel[depth] + numNetsAtThisLevel;

  }  // end for

  threadWorkers.resize(maxLogicalDepth + 1);
  for (int depth = 0; depth <= maxLogicalDepth; depth++) {
    threadWorkers[depth].resize(threadNumThreads);

    for (int i = 0; i < threadNumThreads; i++) {
      const int n0 = threadNetPointers[depth][i];
      const int n1 = threadNetPointers[depth][i + 1];

      threadWorkers[depth][i].setup(this, n0, n1, i);
    }  // end for
  }    // end for

#ifndef NDEBUG

  /*
   for ( int depth = 0; depth <= maxLogicalDepth; depth++ ) {
   cerr << "Depth " << depth << ":\n";
   cerr << "\t#Nets=\t" << (timingOffsetToNetLevel[depth+1] -
   timingOffsetToNetLevel[depth])  << "\n"; cerr << "\tRange=\t" <<
   timingOffsetToNetLevel[depth] << "\t" << timingOffsetToNetLevel[depth+1] <<
   "\n";

   for (int t = 0; t < threadNumThreads; t++ ) {
   const int k0 = threadNetPointers[depth][t];
   const int k1 = threadNetPointers[depth][t+1];

   cerr << "\tThread " << t << "\t" << k0 << "\t" << k1 << "\n";
   }
   }
   */

  // All nets must be covered only once.
  vector<int> covered(timingNets.size(), 0);

  int previousNetIndex = 0;

  for (int depth = 0; depth <= maxLogicalDepth; depth++) {
    for (int i = 0; i < threadNumThreads; i++) {
      const int n0 = threadNetPointers[depth][i];
      const int n1 = threadNetPointers[depth][i + 1];

      for (int n = n0; n < n1; n++) {
        covered[n]++;

        const TimingNet &net = timingNets[n];

        if (net.depth != depth) {
          cerr << "[BUG] @ Circuit::setupMultithreading() - "
               << "Net depth is different than the current depth (" << net.depth
               << " != " << depth << ")\n";
        }  // end if

        if (n < previousNetIndex) {
          cerr << "[BUG] @ Circuit::setupMultithreading() - "
               << "Current net index is less than previous one (" << n
               << " != " << previousNetIndex << ")\n";
        }  // end if

        if (n < timingOffsetToNetLevel.front() ||
            n >= timingOffsetToNetLevel.back()) {
          cerr << "[BUG] @ Circuit::setupMultithreading() - "
               << "Net index is dummy (" << n << ")\n";
        }  // end if

        previousNetIndex = n;
      }  // end for

    }  // end for
  }    // end for

  for (int i = timingOffsetToNetLevel.front();
       i < timingOffsetToNetLevel.back(); i++) {
    if (covered[i] != 1) {
      cerr << "[BUG] @ Circuit::setupMultithreading() - "
           << "Net " << i << " not covered or covered multiple times ("
           << covered[i] << ")\n";
    }  // end if
  }    // end for

#endif

}  // end method

#endif

// -----------------------------------------------------------------------------
/*
 void Circuit::updateTimingSlacks( const int i ) {
 EdgeArray<int>    maxArrivalTimeArc;
 EdgeArray<double> maxArrivalTime(-numeric_limits<double>::max(),
 -numeric_limits<double>::max());

 EdgeArray<int>    maxOutputSlewArc;
 EdgeArray<double> maxOutputSlew(-numeric_limits<double>::max(),
 -numeric_limits<double>::max());

 const int k0 = timingArcPointers[i];
 const int k1 = timingArcPointers[i+1];
 for ( int k = k0; k < k1; k++ ) {
 // Update arc timings.
 TimingArc &arc = timingArcs[k];

 const LibParserTimingInfo &timingInfo =
 arc.cell->actualInstType->timingArcs[arc.lut]; const TimingNet &netDriver =
 timingNets[arc.driver]; const TimingNet &netSink  = timingNets[arc.sink];

 //args:          cellSize,   input_slew,     output_load,  (result),  (result)
 computeArcTiming(timingInfo, netDriver.slew, netSink.load, arc.delay, arc.slew
 );

 // We hope the compiler will unroll this loop for us :)
 for ( int outputEdge = 0; outputEdge < 2; outputEdge++ ) {
 const EdgeType inputEdge = ((EdgeType) outputEdge).getReversed();

 //set worst output arrival time
 const double arrivalTime = netDriver.arrivalTime[inputEdge] +
 arc.delay[outputEdge]; if ( arrivalTime > maxArrivalTime[outputEdge] ) {
 maxArrivalTime[outputEdge] = arrivalTime;
 maxArrivalTimeArc[outputEdge] = k;
 } // end if

 //set worst output slew
 const double outputSlew = arc.slew[outputEdge];
 if ( outputSlew > maxOutputSlew[outputEdge] ) {
 maxOutputSlew[outputEdge] = outputSlew;
 maxOutputSlewArc[outputEdge] = k;
 } // end if

 } // end for
 } // end for

 TimingNet &net = timingNets[i];
 net.arrivalTime = maxArrivalTime;
 net.slew = maxOutputSlew;
 net.backtrack = maxArrivalTimeArc;
 } // end method
 */
// -----------------------------------------------------------------------------

void Circuit::updateTiming() {
  stopwatchUpdateTiming.start();
#ifdef PARALLEL
  updateTimingMultiThreaded();
#else
  updateTimingSingleThreaded();
#endif
  stopwatchUpdateTiming.stop();
}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateTimingSingleThreaded() {
  // Combinational cells.
  const int numNets = timingNets.size();
  for (int i = timingNumDummyNets; i < numNets; i++) updateTiming_Net(i);

  updateTiming_WorstArrivalTime();  // compute the worst arrival time
  updateTiming_SlewViolation();     // thread-safe slew violation update.
  updateTiming_Deprecated();        // keep old stuffs up-to-date.

#ifndef NDEBUG
  updateTiming_Debug();
#endif
}  // end method

// -----------------------------------------------------------------------------

#ifdef PARALLEL

void Circuit::updateTimingMultiThreaded() {
  for (int depth = 0; depth <= maxLogicalDepth; depth++) {
    for (int i = 0; i < threadNumThreads; i++) {
      myThreadPool.start(threadWorkers[depth][i]);
      myThreadPool.joinAll();
    }
    myThreadPool.joinAll();
  }  // end for

  updateTiming_WorstArrivalTime();  // compute the worst arrival time
  updateTiming_SlewViolation();     // thread-safe slew violation update.
  updateTiming_Deprecated();        // keep old stuffs up-to-date.

#ifndef NDEBUG
  updateTiming_Debug();
#endif

}  // end method

#endif

// -----------------------------------------------------------------------------

void Circuit::updateTiming(Vcell *cell) {
  typedef pair<int, int> T;
  priority_queue<T, vector<T>, greater<T>> queue;

  // Update all timing arcs driving the seed net.
  const int k0 = timingArcPointers[cell->sinkNetIndex];
  const int k1 = timingArcPointers[cell->sinkNetIndex + 1];
  for (int k = k0; k < k1; k++) {
    const TimingArc &arc = timingArcs[k];
    // [TODO] I think this if is not necessary anymore as now we have dummy
    // arcs. Remove it after the contest.
    if (arc.driver != -1) {
      const int n = arc.driver;
      updateTiming_Net(n);

      // [TODO] Avoid queuing more than once!
      // Put all sink nets in the queue.
      const int k0 = timingSinkNetPointers[n];
      const int k1 = timingSinkNetPointers[n + 1];
      for (int k = k0; k < k1; k++) {
        const int sink = timingSinkNets[k];
        queue.push(make_pair(timingNets[sink].depth, sink));
      }  // end for
    }    // end if
  }      // end for

  // Propagate arrival times.
  while (!queue.empty()) {
    const int n = queue.top().second;
    queue.pop();

    const TimingNet &net = timingNets[n];
    const TimingNetState &netstate = getTimingNetState(n);

    assert(net.depth > 0);

    const EdgeArray<double> previousSlew = netstate.slew;
    const EdgeArray<double> previousArrivalTime = netstate.arrivalTime;

    updateTiming_Net(n);

    const EdgeArray<double> deltaSlew = netstate.slew - previousSlew;
    const EdgeArray<double> deltaArrivalTime =
        netstate.arrivalTime - previousArrivalTime;

    // If there are some change at timing values propagate...
    if (!nearZero(deltaSlew[FALL]) || !nearZero(deltaSlew[RISE]) ||
        !nearZero(deltaArrivalTime[FALL]) ||
        !nearZero(deltaArrivalTime[RISE])) {
      // Put all sink nets in the queue.
      const int k0 = timingSinkNetPointers[n];
      const int k1 = timingSinkNetPointers[n + 1];
      for (int k = k0; k < k1; k++) {
        const int sink = timingSinkNets[k];
        queue.push(make_pair(timingNets[sink].depth, sink));
      }  // end for
    }    // end if
  }      // end while

  updateTiming_WorstArrivalTime();  // compute the worst arrival time
  updateTiming_SlewViolation();     // thread-safe slew violation update.
  updateTiming_Deprecated();        // keep old stuffs up-to-date.

#ifndef NDEBUG
  updateTiming_Debug();
#endif

}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateTimingLocally(const int n) {
  const int k0 = timingLocalNetPointers[n];
  const int k1 = timingLocalNetPointers[n + 1];
  for (int k = k0; k < k1; k++) {
    updateTiming_Net(timingLocalNets[k]);
  }  // end for

  updateTiming_SlewViolation();
}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateTimingLocallyIncludingSideNets(const int n) {
  updateTimingLocally(n);
  return;

  const int k0 = timingLocalNetPointersIncludingSideNets[n];
  const int k1 = timingLocalNetPointersIncludingSideNets[n + 1];
  for (int k = k0; k < k1; k++) {
    updateTiming_Net(timingLocalNetsIncludingSideNets[k]);
  }  // end for

  updateTiming_SlewViolation();
}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateTimingDriverCell(const int n) {
  /*
       const int k0 = timingArcPointers[n];
       const int k1 = timingArcPointers[n + 1];
       for (int k = k0; k < k1; k++) {
       // Update arc timings.
       TimingArc &arc = timingArcs[k];
       updateTiming_Net(arc.driver);
       } // end for
       */
  updateTiming_Net(n);

  updateTiming_SlewViolation();
}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateTiming_Net(const int i, const int threadId) {
  EdgeArray<int> maxArrivalTimeArc;
  EdgeArray<double> maxArrivalTime(-numeric_limits<double>::max(),
                                   -numeric_limits<double>::max());

  EdgeArray<int> maxOutputSlewArc;
  EdgeArray<double> maxOutputSlew(-numeric_limits<double>::max(),
                                  -numeric_limits<double>::max());

  const TimingNet &net = timingNets[i];
  TimingNetState &netstate = getTimingNetState(i);

  const int k0 = timingArcPointers[i];
  const int k1 = timingArcPointers[i + 1];
  for (int k = k0; k < k1; k++) {
    // Update arc timings.
    const TimingArc &arc = timingArcs[k];
    TimingArcState &arcstate = getTimingArcState(k);

    const LibParserTimingInfo &timingInfo =
        arc.cell->actualInstType->timingArcs[arc.lut];

    assert(arc.sink == i);

    // args:          cellSize, input slew, output load, (result), (result)
    computeArcTiming(timingInfo, arcstate.islew, netstate.load, arcstate.delay,
                     arcstate.oslew);

    // We hope the compiler will unroll this loop for us :)
    for (int outputEdge = 0; outputEdge < 2; outputEdge++) {
      const EdgeType inputEdge = ((EdgeType)outputEdge).getReversed();

      // set worst output arrival time
      const double arrivalTime =
          arcstate.arrivalTime[inputEdge] + arcstate.delay[outputEdge];
      if (arrivalTime > maxArrivalTime[outputEdge]) {
        maxArrivalTime[outputEdge] = arrivalTime;
        maxArrivalTimeArc[outputEdge] = k;
      }  // end if

      // set worst output slew
      const double outputSlew = arcstate.oslew[outputEdge];
      if (outputSlew > maxOutputSlew[outputEdge]) {
        maxOutputSlew[outputEdge] = outputSlew;
        maxOutputSlewArc[outputEdge] = k;
      }  // end if

    }  // end for
  }    // end for

  double &slewViolation = timingViolationSlewVector[threadId];

  slewViolation -= computeSlewViolation(netstate.slew[RISE], net.fanout);
  slewViolation -= computeSlewViolation(netstate.slew[FALL], net.fanout);

  netstate.arrivalTime = maxArrivalTime;
  netstate.slew = maxOutputSlew;
  netstate.backtrack = maxArrivalTimeArc;
  netstate.backtrackSlew = maxOutputSlewArc;

  updateTiming_NetEndpoints(i);

  slewViolation += computeSlewViolation(netstate.slew[RISE], net.fanout);
  slewViolation += computeSlewViolation(netstate.slew[FALL], net.fanout);

}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateTiming_Nets(const int n0, const int n1,
                                const int threadId) {
  for (int n = n0; n < n1; n++) updateTiming_Net(n, threadId);
}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateTiming_NetEndpoints(const int i) {
  // For now just copy the input slew and arrival time to sink timing arcs.

  const TimingNetState &netstate = getTimingNetState(i);

  const int k0 = timingSinkArcPointers[i];
  const int k1 = timingSinkArcPointers[i + 1];
  for (int k = k0; k < k1; k++) {
    // Update arc timings.
    TimingArcState &arcstate = getTimingArcState(timingSinkArcs[k]);

    arcstate.arrivalTime = netstate.arrivalTime;
    arcstate.islew = netstate.slew;
  }  // end for

}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateTiming_WorstArrivalTime() {
  // [NOTE] We could do this while computing arrival times, however it would
  // be a waste of time since we know the worst arrival time occurs only at
  // path tails.

  const double T = getT();

  timingTotalNegativeSlack = 0;
  timingTotalPositiveSlack = 0;
  timingTotalAbsoluteSlack = 0;

  timingWorstArrivalTime.set(-numeric_limits<double>::max(),
                             -numeric_limits<double>::max());
  timingWorstArrivalTimeNet.set(-1, -1);

  timingNumPathsWithNegativeSlack = 0;

  const int numTails = timingTailNets.size();
  for (int i = 0; i < numTails; i++) {
    const int netindex = timingTailNets[i];
    const TimingNetState &netstate = getTimingNetState(netindex);
    for (int edge = 0; edge < 2; edge++) {
      if (netstate.arrivalTime[edge] > timingWorstArrivalTime[edge]) {
        timingWorstArrivalTime[edge] = netstate.arrivalTime[edge];
        timingWorstArrivalTimeNet[edge] = netindex;
      }  // end if

      const double slack =
          timingTailNetMultiplicities[i] * (T - netstate.arrivalTime[edge]);

      if (slack < 0) {
        timingTotalNegativeSlack -= slack;
        timingNumPathsWithNegativeSlack++;
      } else {
        timingTotalPositiveSlack += slack;
      }  // end else

      timingTotalAbsoluteSlack += fabs(slack);
    }  // end for
  }    // end for
}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateTiming_SlewViolation() {
#ifdef PARALLEL
  timingViolationSlew = 0;
  for (int i = 0; i < threadNumThreads; i++)
    timingViolationSlew += timingViolationSlewVector[i];
#else
  timingViolationSlew = timingViolationSlewVector[0];
#endif
}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateLambdasByLiLi() {
  updateLambdas_Subgradient();
  updateLambdas_Normalization();
  updateLambdas_KKT();
}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateLambdasByTennakoon() {
  // T  -> target delay (clock period)
  // Di -> timing arc delay
  // aj -> arrival time (reversed) at arc input
  // ai -> arrival time at arc output, i.e, arrival time at arc's sink net

  // Notice: aj + Di != ai

  const double T = sdcInfos.clk_period;

  // Output Timing Arcs (tail)
  const int numArcs = timingArcs.size();
  for (int i = timingOffsetToExtraSequentialArcs; i < numArcs; i++) {
    TimingArcState &arcstate = getTimingArcState(i);

    const EdgeArray<double> ai = arcstate.arrivalTime;

    arcstate.lambda *= (ai / T);
  }  // end for

  // Combinational Timing Arcs (body)
  for (int i = timingOffsetToCombinationArcs;
       i < timingOffsetToExtraSequentialArcs; i++) {
    const TimingArc &arc = timingArcs[i];
    TimingArcState &arcstate = getTimingArcState(i);

    const TimingNetState &netstate = getTimingNetState(arc.sink);

    const EdgeArray<double> ai = netstate.arrivalTime;
    const EdgeArray<double> aj = arcstate.arrivalTime.getReversed();
    const EdgeArray<double> Di = arcstate.delay;

    arcstate.lambda *= ((aj) / (ai - Di));
  }  // end for

  // Input Timing Arcs (head)
  for (int i = 0; i < timingOffsetToCombinationArcs; i++) {
    const TimingArc &arc = timingArcs[i];
    TimingArcState &arcstate = getTimingArcState(i);

    const TimingNetState &netstate = getTimingNetState(arc.sink);

    const EdgeArray<double> ai = netstate.arrivalTime;
    const EdgeArray<double> Di = arcstate.delay;

    arcstate.lambda *= (Di / ai);
  }  // end for

  updateLambdas_KKT();
}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateLambdasByFlach() {
  updateLambdasByFlachNew();
  return;

  // T  -> target delay (clock period)
  // Di -> timing arc delay
  // aj -> arrival time (reversed) at arc input
  // ai -> arrival time at arc output, i.e, arrival time at arc's sink net

  // Notice: aj + Di != ai

  const double T = sdcInfos.clk_period;

  // Output Timing Arcs (tail)
  const int numArcs = timingArcs.size();
  for (int i = timingOffsetToExtraSequentialArcs; i < numArcs; i++) {
    TimingArcState &arcstate = getTimingArcState(i);

    const EdgeArray<double> a = arcstate.arrivalTime.getReversed();

    arcstate.lambda *= (a / T);
  }  // end for

  // Combinational Timing Arcs (body)
  for (int i = timingOffsetToCombinationArcs;
       i < timingOffsetToExtraSequentialArcs; i++) {
    const TimingArc &arc = timingArcs[i];
    TimingArcState &arcstate = getTimingArcState(i);

    const EdgeArray<double> q = timingRequiredTime[arc.sink];
    const EdgeArray<double> a =
        arcstate.arrivalTime.getReversed() + arcstate.delay;

    arcstate.lambda *= (a / q);
  }  // end for

  // Input Timing Arcs (head)
  for (int i = 0; i < timingOffsetToCombinationArcs; i++) {
    const TimingArc &arc = timingArcs[i];
    TimingArcState &arcstate = getTimingArcState(i);

    const EdgeArray<double> q = timingRequiredTime[arc.sink];
    const EdgeArray<double> a =
        arcstate.arrivalTime.getReversed() + arcstate.delay;

    arcstate.lambda *= (a / q);
  }  // end for

  updateLambdas_KKT();

  //	EdgeArray<double>
  // minLambda(+numeric_limits<double>::max(),+numeric_limits<double>::max());
  //	EdgeArray<double> maxLambda(0,0);
  //	for (int i = 0; i < numArcs; i++) {
  //		minLambda = min( minLambda, getTimingArcState(i).lambda );
  //		maxLambda = max( maxLambda, getTimingArcState(i).lambda );
  //	}
  //	cout << "Min Lambda: " << minLambda << "\n";
  //	cout << "Max Lambda: " << maxLambda << "\n";

}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateLambdasByFlachNew() {
  // T  -> target delay (clock period)
  // Di -> timing arc delay
  // aj -> arrival time (reversed) at arc input
  // ai -> arrival time at arc output, i.e, arrival time at arc's sink net

  // Notice: aj + Di != ai

  const double T = sdcInfos.clk_period;

  const int numArcs = timingArcs.size();
  for (int i = 0; i < numArcs; i++) {
    const TimingArc &arc = timingArcs[i];
    TimingArcState &arcstate = getTimingArcState(i);

    const EdgeArray<double> q =
        arc.sink == -1 ? EdgeArray<double>(T, T) : timingRequiredTime[arc.sink];
    const EdgeArray<double> a =
        arcstate.arrivalTime.getReversed() + arcstate.delay;

    for (int edge = 0, reverseEdge = 1; edge < 2; edge++, reverseEdge--) {
      const double damping = a[edge] > q[edge]
                                 ? 1.0 + ((a[edge] - q[edge]) / T)
                                 : 1.0 / (1.0 + ((q[edge] - a[edge]) / T));

      arcstate.lambda[edge] *= damping;
    }
  }  // end for

  updateLambdas_KKT();

  //	EdgeArray<double>
  // minLambda(+numeric_limits<double>::max(),+numeric_limits<double>::max());
  //	EdgeArray<double> maxLambda(0,0);
  //	for (int i = 0; i < numArcs; i++) {
  //		minLambda = min( minLambda, getTimingArcState(i).lambda );
  //		maxLambda = max( maxLambda, getTimingArcState(i).lambda );
  //	}
  //	cout << "Min Lambda: " << minLambda << "\n";
  //	cout << "Max Lambda: " << maxLambda << "\n";

}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateLambdasByTennakoonFig4(const double stepSize) {
  // T  -> target delay (clock period)
  // Di -> timing arc delay
  // aj -> arrival time (reversed) at arc input
  // ai -> arrival time at arc output, i.e, arrival time at arc's sink net

  // Notice: aj + Di != ai

  const double T = getT();

  // Output Timing Arcs (tail)
  const int numArcs = timingArcs.size();
  for (int i = timingOffsetToExtraSequentialArcs; i < numArcs; i++) {
    TimingArcState &arcstate = getTimingArcState(i);

    const EdgeArray<double> ai = arcstate.arrivalTime;

    arcstate.lambda += stepSize * (ai - T);
  }  // end for

  // Combinational Timing Arcs (body)
  for (int i = timingOffsetToCombinationArcs;
       i < timingOffsetToExtraSequentialArcs; i++) {
    const TimingArc &arc = timingArcs[i];
    TimingArcState &arcstate = getTimingArcState(i);

    const TimingNetState &netstate = getTimingNetState(arc.sink);

    const EdgeArray<double> ai = netstate.arrivalTime;
    const EdgeArray<double> aj = arcstate.arrivalTime.getReversed();
    const EdgeArray<double> Di = arcstate.delay;

    arcstate.lambda += stepSize * (aj + Di - ai);
  }  // end for

  // Input Timing Arcs (head)
  for (int i = 0; i < timingOffsetToCombinationArcs; i++) {
    const TimingArc &arc = timingArcs[i];
    TimingArcState &arcstate = getTimingArcState(i);

    const TimingNetState &netstate = getTimingNetState(arc.sink);

    const EdgeArray<double> ai = netstate.arrivalTime;
    const EdgeArray<double> Di = arcstate.delay;

    arcstate.lambda += stepSize * (Di - ai);
  }  // end for

  updateLambdas_KKT();
}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateLambdas_Subgradient() {
  double beta = 2.0;
  const int numArcs = timingArcs.size();

  // Timing Arcs (head)
  for (int i = timingOffsetToCombinationArcs; i < numArcs; i++) {
    TimingArcState &arcstate = getTimingArcState(i);

    const EdgeArray<double> aj = arcstate.arrivalTime.getReversed();
    const EdgeArray<double> Di = arcstate.delay;

    arcstate.lambda *= pow((aj + Di), beta);
  }  // end for

}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateLambdas_Normalization() {
  const int numTimingArcs = timingArcs.size();

  double sum = 0;
  for (int i = timingOffsetToExtraSequentialArcs; i < numTimingArcs; i++) {
    sum += getTimingArcState(i).lambda.aggregate();
  }  // end for

  for (int i = timingOffsetToExtraSequentialArcs; i < numTimingArcs; i++) {
    getTimingArcState(i).lambda /= sum;
  }  // end for

#ifndef NDEBUG

  sum = 0;
  for (int i = timingOffsetToExtraSequentialArcs; i < numTimingArcs; i++) {
    const TimingArcState &arcstate = getTimingArcState(i);
    sum += arcstate.lambda.aggregate();
  }  // end for

  if (!nearlyEqual(sum, 1.0))
    cerr << "[BUG] @ Circuit::updateLambdas_Normalization() - Output lambda "
            "sum is not equal to 1 (= "
         << sum << ").\n";

#endif
}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateLambdas_KKT() {
  const int numNets = timingNets.size();
  for (int i = numNets - 1; i >= timingNumDummyNets; i--) {
    EdgeArray<double> sumDriverLambdas(0, 0);
    EdgeArray<double> sumSinkLambdas(0, 0);

    const int k0 = timingArcPointers[i];
    const int k1 = timingArcPointers[i + 1];

    const int q0 = timingSinkArcPointers[i];
    const int q1 = timingSinkArcPointers[i + 1];

    // Compute sum of driver timing arc lambdas.
    for (int k = k0; k < k1; k++)
      sumDriverLambdas = sumDriverLambdas + getTimingArcState(k).lambda;

    // Compute sum of sink timing arc lambdas.
    for (int q = q0; q < q1; q++)
      sumSinkLambdas =
          sumSinkLambdas + getTimingArcState(timingSinkArcs[q]).lambda;

    // Update driver arcs.
    for (int edge = 0, reverseEdge = 1; edge < 2; edge++, reverseEdge--) {
      const double sum = sumDriverLambdas[reverseEdge];
      if (sum > 0) {
        for (int k = k0; k < k1; k++) {
          TimingArcState &arcstate = getTimingArcState(k);
          arcstate.lambda[reverseEdge] =
              sumSinkLambdas[edge] * (arcstate.lambda[reverseEdge] / sum);
        }  // end for
      } else {
        const int numSinks = k1 - k0;
        for (int k = k0; k < k1; k++) {
          getTimingArcState(k).lambda[reverseEdge] =
              (sumSinkLambdas[edge] / numSinks);
        }  // end for
      }    // end else
    }      // end for

  }  // end for

  //	cerr << timingOffsetToCombinationArcs << "\n";
  //	cerr << timingOffsetToExtraSequentialArcs << "\n";
  //	cerr << timingOffsetToExtraPrimaryOutputArcs << "\n";
  //	for ( int i = 0; i < timingArcs.size(); i++ ) {
  //		const TimingArcState &arcstate = getTimingArcState(i);
  //
  //		if ( arcstate.lambda[RISE] < 0 || arcstate.lambda[FALL] < 0 )
  //			cerr << i << "\t" << arcstate.lambda << "\n";
  //
  //	}
  //

#ifndef NDEBUG
  for (int i = numNets - 1; i >= timingNumDummyNets; i--) {
    EdgeArray<double> sumDriverLambdas(0, 0);
    EdgeArray<double> sumSinkLambdas(0, 0);

    const int k0 = timingArcPointers[i];
    const int k1 = timingArcPointers[i + 1];

    const int q0 = timingSinkArcPointers[i];
    const int q1 = timingSinkArcPointers[i + 1];

    // Compute sum of driver timing arc lambdas.
    for (int k = k0; k < k1; k++)
      sumDriverLambdas = sumDriverLambdas + getTimingArcState(k).lambda;

    // Compute sum of sink timing arc lambdas.
    for (int q = q0; q < q1; q++)
      sumSinkLambdas =
          sumSinkLambdas + getTimingArcState(timingSinkArcs[q]).lambda;

    // Check lambdas.
    if (!nearlyEqual(sumDriverLambdas.getFall(), sumSinkLambdas.getRise()))
      cerr << "[BUG] @ Circuit::updateLambdas_KKT() - Input rise lambda sum "
              "does not match output fall lambda sum for net "
           << timingNetName[i] << ": " << sumDriverLambdas.getRise()
           << " != " << sumSinkLambdas.getFall() << "\n";

    if (!nearlyEqual(sumDriverLambdas.getRise(), sumSinkLambdas.getFall()))
      cerr << "[BUG] @ Circuit::updateLambdas_KKT() - Input rise lambda sum "
              "does not match output rise lambda sum for net "
           << timingNetName[i] << ": " << sumDriverLambdas.getFall()
           << " != " << sumSinkLambdas.getRise() << "\n";
  }  // end for
#endif

}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateLambdas_Nets() {
  for (int n = timingOffsetToNetLevel.front();
       n < timingOffsetToNetLevel.back(); n++) {
    EdgeArray<double> lambda(0, 0);

    const int k0 = timingSinkArcPointers[n];
    const int k1 = timingSinkArcPointers[n + 1];
    for (int k = k0; k < k1; k++) {
      lambda += getTimingArcState(timingSinkArcs[k]).lambda;
    }  // end for

    getTimingNetState(n).lambdaDelay = lambda;
  }  // end for
}  // end method

// -----------------------------------------------------------------------------

double Circuit::computeNorm() {
  // T  -> target delay (clock period)
  // Di -> timing arc delay
  // aj -> arrival time (reversed) at arc input
  // ai -> arrival time at arc output, i.e, arrival time at arc's sink net

  // Notice: aj + Di != ai
  const double T = getT();
  EdgeArray<double> norm(0, 0);

  // Output Timing Arcs (tail)
  const int numArcs = timingArcs.size();
  for (int i = timingOffsetToExtraSequentialArcs; i < numArcs; i++) {
    TimingArcState &arcstate = getTimingArcState(i);

    const EdgeArray<double> ai = arcstate.arrivalTime;

    norm += pow(ai - T, 2.0);
  }  // end for

  // Combinational Timing Arcs (body)
  for (int i = timingOffsetToCombinationArcs;
       i < timingOffsetToExtraSequentialArcs; i++) {
    const TimingArc &arc = timingArcs[i];
    const TimingArcState &arcstate = getTimingArcState(i);
    const TimingNetState &netstate = getTimingNetState(arc.sink);

    const EdgeArray<double> ai = netstate.arrivalTime;
    const EdgeArray<double> aj = arcstate.arrivalTime.getReversed();
    const EdgeArray<double> Di = arcstate.delay;

    norm += pow(aj + Di - ai, 2.0);
  }  // end for

  // Input Timing Arcs (head)
  for (int i = 0; i < timingOffsetToCombinationArcs; i++) {
    const TimingArc &arc = timingArcs[i];
    const TimingArcState &arcstate = getTimingArcState(i);
    const TimingNetState &netstate = getTimingNetState(arc.sink);

    const EdgeArray<double> ai = netstate.arrivalTime;
    const EdgeArray<double> Di = arcstate.delay;

    norm += pow(Di - ai, 2.0);
  }  // end for

  return norm.aggregate();
}  // end method

// -----------------------------------------------------------------------------

double Circuit::computeStepSize(const double UB, const double L) {
  const double norm = computeNorm();
  return (UB - L) / norm;
}  // end method

// -----------------------------------------------------------------------------

double Circuit::computeLagrange() {
  double L = totalLeakage;

  for (int i = timingOffsetToCombinationArcs;
       i < timingOffsetToExtraSequentialArcs; i++) {
    const TimingArcState &arcstate = getTimingArcState(i);
    L += (arcstate.delay * arcstate.lambda).aggregate();
  }  // end for

  // [TODO] Consider some discarded constants here :)

  return L;
}  // end method

// -----------------------------------------------------------------------------

void Circuit::resetTimingArcLambdas(const double value) {
  const int numTimingArcs = timingArcs.size();
  for (int i = 0; i < numTimingArcs; i++)
    getTimingArcState(i).lambda.set(value, value);
}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateRequiredTime() {
  const double T = sdcInfos.clk_period;
  const int numNets = timingNets.size();

  // Set the required time for all nets to clock period.
  for (int i = timingNumDummyNets; i < numNets; i++)
    timingRequiredTime[i].set(T, T);

  // Propagate back the required time.
  for (int i = numNets - 1; i >= timingNumDummyNets; i--) {
    const EdgeArray<double> &requiredTimeAtSink = timingRequiredTime[i];

    const int k0 = timingArcPointers[i];
    const int k1 = timingArcPointers[i + 1];
    for (int k = k0; k < k1; k++) {
      // Update arc timings.
      const TimingArc &arc = timingArcs[k];
      TimingArcState &arcstate = getTimingArcState(k);

      EdgeArray<double> &requiredTimeAtDriver = timingRequiredTime[arc.driver];
      // Reimann
      EdgeArray<double> slack;
      /////////
      for (int edge = 0; edge < 2; edge++) {
        const EdgeType reverseEdge = ((EdgeType)edge).getReversed();

        const double requiredTime =
            requiredTimeAtSink[edge] - arcstate.delay[edge];
        if (requiredTimeAtDriver[reverseEdge] > requiredTime)
          requiredTimeAtDriver[reverseEdge] = requiredTime;
        // Reimann: -m(u->v)
        slack[edge] = requiredTimeAtSink[reverseEdge] -
                      (getTimingNetState(arc.driver).arrivalTime[edge] +
                       arcstate.delay[reverseEdge]);

      }  // end for
      // Reimann
      arcstate.slack = slack;
    }  // end for
  }    // end for

#ifndef NDEBUG
  if (getWorstSlack() >= 0) {
    for (int i = numNets - 1; i >= timingNumDummyNets; i--) {
      if (getNetSlack(i).getMin() < 0)
        cerr << "[BUG] @ updateRequiredTime(): Worst Slack = "
             << getWorstSlack()
             << " but Net Slack = " << getNetSlack(i).getMin() << "\n";
    }  // end for
  }    // end if
#endif

}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateLambdaDelaySensitivities() {
  const int numNets = timingNets.size();
  const int numArcs = timingArcs.size();

  timingArcLambdaDelaySensitivity.assign(numArcs, EdgeArray<double>(0, 0));

  // Propagate back the required time.
  for (int i = numNets - 1; i >= timingNumDummyNets; i--) {
    const TimingNet &net = timingNets[i];
    const TimingNetState &netstate = getTimingNetState(i);

    EdgeArray<double> accumulatedSensitivity(0, 0);

    {  // Compute accumulated sensitivities up to this net.
      const int k0 = timingSinkArcPointers[i];
      const int k1 = timingSinkArcPointers[i + 1];
      for (int k = k0; k < k1; k++) {
        accumulatedSensitivity +=
            timingArcLambdaDelaySensitivity[timingSinkArcs[k]];
      }  // end for
    }    // end block

    timingNetLambdaDelaySensitivity[i] = accumulatedSensitivity;

    const int k0 = timingArcPointers[i];
    const int k1 = timingArcPointers[i + 1];
    for (int k = k0; k < k1; k++) {
      // Update arc timings.
      const TimingArcState &arcstate = getTimingArcState(k);

      EdgeArray<double> sensitivityDelay;
      EdgeArray<double> sensitivitySlew;
      computeArcTimingDelayAndSlewSensitivityToInputSlew(
          k, net.driver->actualInstTypeIndex, sensitivityDelay,
          sensitivitySlew);

      timingArcLambdaDelaySensitivity[k] +=
          (arcstate.lambda * sensitivityDelay).getReversed();

      for (int edge = 0; edge < 2; edge++) {
        const EdgeType reverseEdge = ((EdgeType)edge).getReversed();
        if (k == netstate.backtrackSlew[edge])
          timingArcLambdaDelaySensitivity[k][reverseEdge] +=
              sensitivitySlew[edge] * accumulatedSensitivity[edge];
      }  // end for
    }    // end for
  }      // end for

  //	for ( int i = 0; i < numArcs; i++ )  {
  //		if ( i == timingOffsetToCombinationArcs )
  //			cout << "### Combinational ###\n";
  //
  //		if ( i == timingOffsetToExtraSequentialArcs )
  //			cout << "### Sequential ###\n";
  //
  //		if ( i == timingOffsetToExtraPrimaryOutputArcs )
  //			cout << "### Outputs ###\n";
  //
  //		cout << timingLambdaDelaySensitivity[i] << "\t" <<
  // timingArcs[i].cell->instType << "\t" <<
  //(i<timingOffsetToCombinationArcs?"dummy":"") << "\n"; 	} // end for
  // exit(1);

}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateTiming_Deprecated() {
  // Deprecated.
  slewViol = timingViolationSlew;
  worstSlack = getWorstSlack();
}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateTiming_Debug() {
  // Check loads.
  for (int i = timingNumDummyNets; i < timingNets.size(); i++) {
    const TimingNet &net = timingNets[i];
    const TimingNetState &netstate = getTimingNetState(i);
    const Vcell *cell = net.driver;

    if (!nearlyEqual(netstate.load, cell->actualLoad)) {
      cerr << "[BUG] @ Circuit::updateTiming() - Divergent total load for net "
           << i << " and driver cell " << cell->instName << " ("
           << cell->instType << ").\n";
      cerr << "\tNet  Total Load: " << netstate.load << "\n";
      cerr << "\tCell Total Load: " << cell->actualLoad << "\n";
      assert(false);
    }  // end if
  }    // end for

  // Check arrival times.
#ifdef COMPARE_TIMING_ENGINES

  for (int i = timingNumDummyNets; i < timingNets.size(); i++) {
    const TimingNet &net = timingNets[i];
    const Vcell *cell = net.driver;

    EdgeArray<double> arrivalTime(sdcInfos.clk_period - cell->actualRiseSlack,
                                  sdcInfos.clk_period - cell->actualFallSlack);

    for (int edge = 0; edge < 2; edge++) {
      if (!nearlyEqual(arrivalTime[edge], net.arrivalTime[edge])) {
        cerr << "[BUG] @ Circuit::updateTiming() - "
             << "Divergent net (" << net.arrivalTime[edge] << ") and "
             << "cell arrival time (" << cell->outputDelay[edge] << ") "
             << "(" << arrivalTime[edge] << ") "
             << "at edge " << ((EdgeType)edge) << ".\n";

        cerr << "\tCell: " << cell->vectorIndex << "\t" << cell->instName
             << "\t" << cell->instType << "\n";
        cerr << "\tNet: " << timingNetName[i] << "\t" << net.depth << "\n";
        /*
saveSizes();
char nada;
cin >> nada;
*/
        assert(false);
      }  // end if
    }    // end for
  }      // end for

  if (!nearlyEqual(timingTotalNegativeSlack, timingViol, 1e-2)) {
    cerr << "[BUG] @ Circuit::updateTiming() - Divergent timing violation.\n";
    cerr << "\tupdateTiming():" << timingTotalNegativeSlack << "\n";
    cerr << "\tcalcTiming()..:" << timingViol << "\n";
    cerr << "\tDelta.........:" << fabs(timingViol - timingTotalNegativeSlack)
         << "\n";
  }  // end if

  if (!nearlyEqual(timingViolationSlew, slewViol, 1e-2)) {
    cerr << "[BUG] @ Circuit::updateTiming() - Divergent slew violation.\n";
    cerr << "\tupdateTiming():" << timingViolationSlew << "\n";
    cerr << "\tcalcTiming()..:" << slewViol << "\n";
    cerr << "\tDelta.........:" << fabs(slewViol - timingViolationSlew) << "\n";
  }  // end if

#endif

  // Check if the critical path delays matches the worst arrival time.
  EdgeType edgeType =
      (timingWorstArrivalTime[FALL] > timingWorstArrivalTime[RISE]) ? FALL
                                                                    : RISE;

  double criticalPathDelay = 0;

  int n = timingWorstArrivalTimeNet[edgeType];
  while (n >= timingNumDummyNets) {
    const TimingNetState &netstate = getTimingNetState(n);

    const TimingArc &arc = timingArcs[netstate.backtrack[edgeType]];
    const TimingArcState &arcstate =
        getTimingArcState(netstate.backtrack[edgeType]);
    criticalPathDelay += arcstate.delay[edgeType];

    n = arc.driver;
    edgeType.reverse();
  }  // end while

  criticalPathDelay +=
      getTimingNetState(n).arrivalTime[edgeType];  // add input driver delay
                                                   // time (when necessary)

  if (!nearlyEqual(criticalPathDelay, timingWorstArrivalTime.getMax())) {
    if (!nearlyEqual(criticalPathDelay, timingWorstArrivalTime.getMax(),
                     1e-2)) {
      cerr << "[BUG] @ Circuit::updateTiming() - "
           << "Critical path delay (" << criticalPathDelay
           << ") does not match "
           << "worst arrival time (" << timingWorstArrivalTime.getMax()
           << ").\n";
      assert(false);
    } else {
      cerr << "[WARNING] @ Circuit::updateTiming() - "
           << "Critical path delay does not match worst arrival time with "
              "EPSILON precision.\n";
      cerr << "\tDiff...: "
           << fabs(criticalPathDelay - timingWorstArrivalTime.getMax()) << "\n";
      cerr << "\tEPSILON: " << EPSILON << "\n";
    }  // end if
  }    // end if

}  // end method

// -----------------------------------------------------------------------------

double Circuit::updateCellAndComputeSizingEffectOnDriverCellDelay(
    Vcell *cell, const int newSize) {
  //*
  EdgeArray<double> effect(0, 0);
  vector<EdgeArray<double>> delays;

  updateTimingDriverCell(cell->sinkNetIndex);
  const int k0 = timingArcPointers[cell->sinkNetIndex];
  const int k1 = timingArcPointers[cell->sinkNetIndex + 1];
  for (int k = k0; k < k1; k++) {
    const TimingArcState &arcstate = getTimingArcState(k);
    // cout << arc.lambda << "\t" << arc.delay << "\t" << k0 << "\t" << k1 <<
    // "\t" ;
    delays.push_back(arcstate.delay);
  }  // end for

  updateCellType(cell, newSize);
  updateTimingDriverCell(cell->sinkNetIndex);

  for (int k = k0; k < k1; k++) {
    const TimingArcState &arcstate = getTimingArcState(k);
    // cout << arc.lambda << "\t" << arc.delay << "\t" << k0 << "\t" << k1 <<
    // "\t" ;
    effect += arcstate.lambda * (arcstate.delay - delays[k - k0]);
  }  // end for

  return effect.aggregate();
}
// -----------------------------------------------------------------------------

double Circuit::computeSizingEffectOnDriverCellDelay(const int netIndex) {
  updateTimingDriverCell(netIndex);
  EdgeArray<double> effect(0, 0);

  const int k0 = timingArcPointers[netIndex];
  const int k1 = timingArcPointers[netIndex + 1];
  for (int k = k0; k < k1; k++) {
    const TimingArcState &arcstate = getTimingArcState(k);
    effect += arcstate.lambda * arcstate.delay;
  }  // end for

  return effect.getMax();
}
// -----------------------------------------------------------------------------

bool Circuit::hasNegativeSlack(const int netIndex) {
  const int k0 = timingArcPointers[netIndex];
  const int k1 = timingArcPointers[netIndex + 1];
  for (int k = k0; k < k1; k++) {
    const TimingArc &arc = timingArcs[k];
    const TimingArcState &arcstate = getTimingArcState(k);
    if (arcstate.slack.getMin() < 0.0) return true;
  }  // end for

  return false;
}
// -----------------------------------------------------------------------------

double Circuit::computeSizingEffectOnDriverCellFanoutDelay(const int netIndex) {
  const TimingNet &net = timingNets[netIndex];
  const int n0 = timingSinkNets[timingSinkNetPointers[netIndex]];
  const int n1 = timingSinkNets[timingSinkNetPointers[netIndex + 1]];
  for (int n = n0; n < n1; n++) updateTimingDriverCell(n);

  EdgeArray<double> effect(0, 0);

  const int k0 = timingSinkArcPointers[netIndex];
  const int k1 = timingSinkArcPointers[netIndex + 1];
  for (int k = k0; k < k1; k++) {
    const TimingArcState &arcstate = getTimingArcState(k);
    effect += arcstate.lambda * arcstate.delay;
  }  // end for

  return effect.aggregate();
}  // end method

// -----------------------------------------------------------------------------

double Circuit::computeSizingEffectOnLambdaDelay(const int netIndex) {
  EdgeArray<double> effect(0, 0);

  const int k0 = timingLocalArcPointers[netIndex];
  const int k1 = timingLocalArcPointers[netIndex + 1];
  for (int k = k0; k < k1; k++) {
    const TimingArcState &arcstate = getTimingArcState(timingLocalArcs[k]);
    effect += arcstate.lambda * arcstate.delay;
  }  // end for

  return effect.aggregate();
}  // end method

// -----------------------------------------------------------------------------

double Circuit::computeSizingEffectOnLambdaDelaySensitivities(
    const int netIndex) {
  EdgeArray<double> effect(0, 0);

  if (optEnableLambdaDelaySensitivities) {
    {  // Driver nets
      const int i0 = timingDriverNetPointers[netIndex];
      const int i1 = timingDriverNetPointers[netIndex + 1];
      for (int i = i0; i < i1; i++) {
        const int n = timingDriverNets[i];
        const EdgeArray<double> deltaSlew =
            getTimingNetState(n).slew - timingPreviousNetSlew[n];
        effect += deltaSlew * timingNetLambdaDelaySensitivity[n];
      }  // end for
    }    // end block

    {  // Arcs of central cell.
      const int k0 = timingArcPointers[netIndex];
      const int k1 = timingArcPointers[netIndex + 1];
      for (int k = k0; k < k1; k++) {
        const TimingArc &arc = timingArcs[k];

        const EdgeArray<double> deltaSlew = getTimingNetState(arc.driver).slew -
                                            timingPreviousNetSlew[arc.driver];
        effect -= deltaSlew * timingArcLambdaDelaySensitivity[k];
      }  // end for
    }    // end block
  }      // end if

  const int k0 = timingLocalArcPointers[netIndex];
  const int k1 = timingLocalArcPointers[netIndex + 1];
  for (int k = k0; k < k1; k++) {
    const TimingArcState &arcstate = getTimingArcState(timingLocalArcs[k]);
    effect += arcstate.lambda * arcstate.delay;
  }  // end for

  if (optEnableLambdaDelaySensitivities) {
    {  // Driver nets
      const int i0 = timingSinkNetPointers[netIndex];
      const int i1 = timingSinkNetPointers[netIndex + 1];
      for (int i = i0; i < i1; i++) {
        const int n = timingSinkNets[i];
        const EdgeArray<double> deltaSlew =
            getTimingNetState(n).slew - timingPreviousNetSlew[n];
        effect += deltaSlew * timingNetLambdaDelaySensitivity[n];
      }  // end for
    }    // end block
  }      // end if

  return effect.aggregate();
}  // end method

// -----------------------------------------------------------------------------

double Circuit::computeSizingEffectOnSlack(const int netIndex) {
  EdgeArray<double> effect = getNetSlack(netIndex);

  // [NOTE] As different inputs may be driven by the same net, we must skip
  // duplicates, hence the use of a set structure :) We can think a more
  // efficient way, but remember to keep it thread safe.

  // Process previous and current level timing arcs.
  set<int> driverNetIndexes;

  {  // Process driver nets.
    const int k0 = timingArcPointers[netIndex];
    const int k1 = timingArcPointers[netIndex + 1];
    for (int k = k0; k < k1; k++) driverNetIndexes.insert(timingArcs[k].driver);

    for (set<int>::const_iterator it = driverNetIndexes.begin();
         it != driverNetIndexes.end(); it++) {
      effect += getNetSlack(*it);
    }  // end for
  }    // end block

  {  // Process next level timing arcs.
    const int k0 = timingSinkNetPointers[netIndex];
    const int k1 = timingSinkNetPointers[netIndex + 1];
    for (int k = k0; k < k1; k++) {
      effect += getNetSlack(timingSinkNets[k]);
    }  // end for
  }    // end block

  return effect.aggregate();
}  // end method

// -----------------------------------------------------------------------------

// double Circuit::computeSizingEffectOnSlackLiLi(const int netIndex) {
//     EdgeArray<double> effect(0, 0);
//
//     // [NOTE] As different inputs may be driven by the same net, we must skip
//     // duplicates, hence the use of a set structure :) We can think a more
//     // efficient way, but remember to keep it thread safe.
//
//     // Process previous and current level timing arcs.
//     set<int> driverNetIndexes;
//
//     const int k0 = timingArcPointers[netIndex];
//     const int k1 = timingArcPointers[netIndex + 1];
//     for (int k = k0; k < k1; k++) {
//         driverNetIndexes.insert(timingArcs[k].driver);
//     } // end for
//
//     EdgeArray<double> maxAggregatedDelay(0,0);
//
//     for (set<int>::const_iterator it = driverNetIndexes.begin(); it !=
//     driverNetIndexes.end(); it++) {
//         const int driverNetIndex = *it;
//
//         EdgeArray<double> maxDelay(0,0);
//
//         // Driver arcs.
//         const int driver0 = timingArcPointers[driverNetIndex];
//         const int driver1 = timingArcPointers[driverNetIndex + 1];
//         for (int k = driver0; k < driver1; k++) {
//             const TimingArc &arc = timingArcs[k];
//             maxDelay = max(maxDelay, arc.pastDelay - arc.delay);
//         } // end for
//		maxDelay.reverse();
//
//         // Sink arcs.
//         const int sink0 = timingSinkArcPointers[driverNetIndex];
//         const int sink1 = timingSinkArcPointers[driverNetIndex + 1];
//         for (int k = sink0; k < sink1; k++) {
//             // [NOTE] Dummy output endpoint arcs have zero delay, so they
//             should
//             // transparently be ignored.
//             const TimingArc &arc = timingArcs[timingSinkArcs[k]];
//
//             // Skip sinbling arcs.
//             if ( arc.driver != netIndex)
//                 continue;
//
//             maxAggregatedDelay = max(maxAggregatedDelay, maxDelay +
//             arc.delay);
//         } // end for
//     } // end for
//
//     // Process next level timing arcs.
//	EdgeArray<double> maxFinalDelay(0,0);
//
//     const int sink0 = timingSinkArcPointers[netIndex];
//     const int sink1 = timingSinkArcPointers[netIndex + 1];
//     for (int k = sink0; k < sink1; k++) {
//         // [NOTE] Dummy output endpoint arcs have zero delay, so they should
//         // transparently be ignored.
//
//         const TimingArc &arc = timingArcs[timingSinkArcs[k]];
//         maxFinalDelay = max(maxFinalDelay, arc.delay.getReversed());
//     } // end for
//
//     return -maxFinalDelay.getMin();
// } // end method

// -----------------------------------------------------------------------------

double Circuit::computeSizingEffectOnLocalNegativeSlack(const int netIndex) {
  EdgeArray<double> effect(0, 0);

  const int k0 = timingLocalNetPointers[netIndex];
  const int k1 = timingLocalNetPointers[netIndex + 1];
  for (int k = k0; k < k1; k++) {
    effect += getNetNegativeSlack(timingLocalNets[k]);
  }  // end for

  return effect.aggregate();
}  // end method

// -----------------------------------------------------------------------------

double Circuit::computeSizingEffectOnAbsoluteSlack(const int netIndex) {
  EdgeArray<double> effect(0, 0);

  const int k0 = timingLocalNetPointers[netIndex];
  const int k1 = timingLocalNetPointers[netIndex + 1];
  for (int k = k0; k < k1; k++) {
    effect += abs(getNetSlack(timingLocalNets[k]));
  }  // end for

  return effect.aggregate();
}  // end method

// -----------------------------------------------------------------------------

double Circuit::computeSizingEffectOnArrivalTimeVariance(const int netIndex) {
  EdgeArray<double> effect(0, 0);

  const int k0 = timingLocalNetPointers[netIndex];
  const int k1 = timingLocalNetPointers[netIndex + 1];
  for (int k = k0; k < k1; k++) {
    effect += getNetArrivalTimeStandardDeviation(timingLocalNets[k]);
  }  // end for

  return effect.aggregate();
}

// -----------------------------------------------------------------------------

void Circuit::assignLogicalEffort() {
  // vector< pair<string, double> > footprintLEPairs;

  for (int j = 0; j < orgCells.oCells.size(); ++j) {
    // footprintLEPairs[j].first = orgCells.oCells[j].footprint;
    switch (j) {
      case 0:  // in01
        footprintLEPairs.push_back(make_pair(orgCells.oCells[j].footprint, 1));
        break;
      case 1:  // na02
        footprintLEPairs.push_back(
            make_pair(orgCells.oCells[j].footprint, 1.3333));
        break;
      case 2:  // na03
        footprintLEPairs.push_back(
            make_pair(orgCells.oCells[j].footprint, 1.6666));
        break;
      case 3:  // na04
        footprintLEPairs.push_back(make_pair(orgCells.oCells[j].footprint, 2));
        break;
      case 4:  // no02
        footprintLEPairs.push_back(
            make_pair(orgCells.oCells[j].footprint, 1.6666));
        break;
      case 5:  // no03
        footprintLEPairs.push_back(
            make_pair(orgCells.oCells[j].footprint, 2.3333));
        break;
      case 6:  // no04
        footprintLEPairs.push_back(make_pair(orgCells.oCells[j].footprint, 3));
        break;
      case 7:  // ao12
        footprintLEPairs.push_back(make_pair(orgCells.oCells[j].footprint, 2));
        break;
      case 8:  // ao22
        footprintLEPairs.push_back(make_pair(orgCells.oCells[j].footprint, 2));
        break;
      case 9:  // oa12
        footprintLEPairs.push_back(make_pair(orgCells.oCells[j].footprint, 2));
        break;
      case 10:  // oa22
        footprintLEPairs.push_back(make_pair(orgCells.oCells[j].footprint, 2));
        break;
      case 11:  // ms00
        footprintLEPairs.push_back(make_pair(orgCells.oCells[j].footprint, 1));
        break;
      case 12:  // vcc
        footprintLEPairs.push_back(make_pair(orgCells.oCells[j].footprint, 1));
        break;
      case 13:  // vss
        footprintLEPairs.push_back(make_pair(orgCells.oCells[j].footprint, 1));
        break;
      default:
        break;
    }
    // cout << " Cell footprint: " << footprintLEPairs[j].first << " LE " <<
    // footprintLEPairs[j].second << endl;
  }
}  // end method

// -----------------------------------------------------------------------------

void Circuit::assignMinimumSlew() {
  const int numOCells = orgCells.oCells.size();

  for (int j = 0; j < (numOCells); ++j) {
    if (j == 11 || j == 12 || j == 13) {
      orgCells.oCells[j].minimumFallSlew = 0;
      orgCells.oCells[j].minimumRiseSlew = 0;
      orgCells.oCells[j].maximumFallSlew = 0;
      orgCells.oCells[j].maximumRiseSlew = 0;
      return;
    }  // end if
    orgCells.oCells[j].minimumFallSlew = numeric_limits<double>::max();
    orgCells.oCells[j].minimumRiseSlew = numeric_limits<double>::max();

    // maximum slew
    orgCells.oCells[j].maximumFallSlew = -numeric_limits<double>::max();
    orgCells.oCells[j].maximumRiseSlew = -numeric_limits<double>::max();

    const int numCandidateCells = orgCells.oCells[j].cells.size();

    for (int i = 0; i < numCandidateCells; i++) {
      const LibParserCellInfo &cell = orgCells.oCells[j].cells[i];

      const int numTimingArcs = cell.timingArcs.size();
      for (int k = 0; k < numTimingArcs; k++) {
        const LibParserTimingInfo &timingInfo = cell.timingArcs[k];

        const double fallTransition = timingInfo.fallTransition.tableVals[0][0];
        const double riseTransition = timingInfo.riseTransition.tableVals[0][0];

        // const double maximumFallTransition =
        // timingInfo.fallTransition.tableVals[6][7]; //The maximum transition
        // is set by maxTransition available in the liberty file const double
        // maximumRiseTransition = timingInfo.riseTransition.tableVals[6][7];
        // //The maximum transition is set by maxTransition available in the
        // liberty file

        orgCells.oCells[j].minimumFallSlew =
            min(orgCells.oCells[j].minimumFallSlew, fallTransition);
        orgCells.oCells[j].minimumRiseSlew =
            min(orgCells.oCells[j].minimumRiseSlew, riseTransition);

        // orgCells.oCells[j].maximumFallSlew =
        // max(orgCells.oCells[j].maximumFallSlew, maxTransition);
        // orgCells.oCells[j].maximumRiseSlew =
        // max(orgCells.oCells[j].maximumRiseSlew, maxTransition);
        orgCells.oCells[j].maximumFallSlew = maxTransition;
        orgCells.oCells[j].maximumRiseSlew = maxTransition;
      }  // end for

    }  // end for
       // cout << "Minimo FALL " << orgCells.oCells[j]. footprint << "\t" <<
    // orgCells.oCells[j].minimumFallSlew << " minimo RISE " <<
    // orgCells.oCells[j].minimumRiseSlew << endl;
    // cout << "Maximo FALL " << orgCells.oCells[j]. footprint << "\t" <<
    // orgCells.oCells[j].maximumFallSlew << " maximo RISE " <<
    // orgCells.oCells[j].maximumRiseSlew << endl;
  }
}  // end method

// -----------------------------------------------------------------------------

void Circuit::assignLogicalEffortContestLibrary() {
  // vector< pair<string, double> > footprintLEPairs;
  //  Considering the relation of the transistor sizes presented to cell size
  //  "8"
  for (int j = 0; j < orgCells.oCells.size(); ++j) {
    // footprintLEPairs[j].first = orgCells.oCells[j].footprint;
    switch (j) {
      case 0:  // in01
        footprintLEPairs.push_back(make_pair(orgCells.oCells[j].footprint, 1));
        break;
      case 1:  // na02
        footprintLEPairs.push_back(
            make_pair(orgCells.oCells[j].footprint, 0.75));
        break;
      case 2:  // na03
        footprintLEPairs.push_back(
            make_pair(orgCells.oCells[j].footprint, 0.6875));
        break;
      case 3:  // na04
        footprintLEPairs.push_back(
            make_pair(orgCells.oCells[j].footprint, 0.625));
        break;
      case 4:  // no02
        footprintLEPairs.push_back(
            make_pair(orgCells.oCells[j].footprint, 0.75));
        break;
      case 5:  // no03
        footprintLEPairs.push_back(
            make_pair(orgCells.oCells[j].footprint, 0.6875));
        break;
      case 6:  // no04
        footprintLEPairs.push_back(
            make_pair(orgCells.oCells[j].footprint, 0.625));
        break;
      case 7:  // ao12
        footprintLEPairs.push_back(make_pair(orgCells.oCells[j].footprint, 1));
        break;
      case 8:  // ao22
        footprintLEPairs.push_back(make_pair(orgCells.oCells[j].footprint, 1));
        break;
      case 9:  // oa12
        footprintLEPairs.push_back(make_pair(orgCells.oCells[j].footprint, 1));
        break;
      case 10:  // oa22
        footprintLEPairs.push_back(make_pair(orgCells.oCells[j].footprint, 1));
        break;
      case 11:  // ms00
        footprintLEPairs.push_back(make_pair(orgCells.oCells[j].footprint, 1));
        break;
      case 12:  // vcc
        footprintLEPairs.push_back(make_pair(orgCells.oCells[j].footprint, 1));
        break;
      case 13:  // vss
        footprintLEPairs.push_back(make_pair(orgCells.oCells[j].footprint, 1));
        break;
      default:
        break;
    }
    // cout << " Cell footprint: " << footprintLEPairs[j].first << " LE " <<
    // footprintLEPairs[j].second << endl;
    cout << "celula " << orgCells.oCells[j].footprint << " j " << j << endl;
  }
}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateTopCriticalTailNets() {
  const int numTailNets = timingTailNets.size();

  // cout << "Numero de tail nets " << numTailNets << endl;
  for (int i = 0; i < numTailNets; i++) {
    const int n = timingTailNets[i];
    const TimingNetState &netstate = getTimingNetState(n);

    arrivalTimingNet.insert(
        make_pair(netstate.arrivalTime[RISE], make_pair(n, RISE)));
    arrivalTimingNet.insert(
        make_pair(netstate.arrivalTime[FALL], make_pair(n, FALL)));
  }  // end for

  // for ( multimap<double, int>::iterator it = arrivalTimingNet.begin(); it !=
  // arrivalTimingNet.end(); it++ ) {
  // cout << "Arrival time: " << it->first << " net " << it->second << endl;
  // } // end for

}  // end method

// -----------------------------------------------------------------------------

void Circuit::loadSizes() {
  FILE *inputSizes;
  string filename = rootDir + "/" + benchName + "/" + benchName + ".sizes2";

  cout << endl << " Loading sizes..." << endl;

  if ((inputSizes = fopen(filename.c_str(), "r"))) {
    char cell[200], size[200];

    set<pair<string, string>> loadSizes;

    while (!feof(inputSizes)) {
      fscanf(inputSizes, "%s %s\n", cell, size);
      loadSizes.insert(make_pair(string(cell), string(size)));
      // cout << string(cell) << " " << string(size) << endl;
    }
    fclose(inputSizes);

    for (int i = offsetCombinational; i < depthSortedCells.size(); ++i) {
      for (int j = 0; j < 30; ++j) {
        Vcell *cell = depthSortedCells[i];
        if (loadSizes.find(make_pair(
                cell->instName,
                orgCells.oCells[cell->footprintIndex].cells[j].name)) !=
            loadSizes.end()) {
          updateCellType(cell, j);
          // cout << "cell " << cell->instName << " changed to: " <<
          // orgCells.oCells[cell->footprintIndex].cells[j].name << endl;
          break;
        }
      }
    }

    cout << " Loading sizes...done" << endl;
  } else
    cerr << "Problem loading .sizes file!" << endl;

  updateTiming();
  printTiming();

}  // end method

// -----------------------------------------------------------------------------

void Circuit::lowTemperatureAnneal() {
  const long int clo = clock();
  struct timeval tp;
  double start, step;

  // Time stamp before the computations
  gettimeofday(&tp, NULL);
  start = static_cast<double>(tp.tv_sec);
  // start = clock() / CLOCKS_PER_SEC;

  const long double minLeakage = this->totalLeakage;
  const double timeLimit =
      5 * 60 * 60 + 1 * 60 * 60 * ceil(this->getSize() / 35000.0);

  const int circuitSize = this->getSize();
  // const int iterationLimit = 10 * 10000000 + 2500 * circuitSize;
  // cout << " Iteration limit: " << iterationLimit << endl;

  this->maxTimingViol =
      this->getPathTailsSize() * (this->getClkPeriod() * 0.50) + 1000.0;
  this->maxWorstSlack = this->worstSlack;

  // double decay = 0.999;
  long double temp;
  const double finalTemp = 1.0e-4;
  // decay = 1.0-1.0/((100000+circuitSize)*20.0);
  double const initialTemp = 0.010;
  /*
   cout << " Initial temperature: " << temp << endl;
   cout << " Initial temperature decay: " << decay << endl;
   cout << " Final temperature: " << finalTemp << endl;
   */
  double lastBestLeakage = this->totalLeakage;
  double lastBestLeakage2 = this->totalLeakage;

  gettimeofday(&tp, NULL);
  step = static_cast<double>(tp.tv_sec);
  // step = clock() / CLOCKS_PER_SEC;
  /*
   this->solveLoadViol();
   this->updateTiming();
   */
  bool increasing = true;
  cout << "\nIterative sizing 1/1..." << endl;
  bool cont = true, cont1, cont2;
  const long unsigned int offset1 = 1;  // circuitSize * 10.0;
  const long unsigned int offset2 = circuitSize * 2.0 + offset1;
  const double fallTime = (circuitSize) * 15.0;
  const double riseTime = (circuitSize) * 1.0;

  const long unsigned int fewChangesCount = circuitSize / 100 + 1000;

  long unsigned int iteration = circuitSize;
  long unsigned int iteration2 = circuitSize;

  int ctrl = iteration2;
  int ctrl2 = iteration2;
  int ctrl3 = iteration2;
  int ctrl4 = iteration;
  int ctrl5 = iteration2;
  int lastChange = iteration;
  int lastChange2 = iteration2;

  const int numTails = timingTailNets.size();
  int inc = 0;

  const double myClock = this->getClkPeriod();
  this->maxTimingViol = (myClock * 0.01) * numTails;
  cout << " Maximum timing violation sum: " << maxTimingViol << endl;

  printTimingDigest(true, "Low Temperature SA");

  Vcell *tmp;
  while (cont) {
    cont1 = true;
    if (iteration2 < offset1 + circuitSize) {
      temp = 1.0e-5;
      iteration2 += inc;
      tmp = *cellIterator;
      while (tmp->logicalDepth <
             this->depthSortedCells.back()->logicalDepth / 2) {
        ++cellIterator;
        tmp = *cellIterator;
        // cout << "Cell iterator if 1 = " << tmp->instName << endl;
      }
      //    if (step-start > timeLimit*0.9)
      //       iteration += circuitSize/20;
    } else if (iteration2 < offset2 + circuitSize) {
      temp = 10.0e-4;
      iteration2 += inc;
      tmp = *cellIterator;
      while (tmp->logicalDepth <
             this->depthSortedCells.back()->logicalDepth / 4) {
        ++cellIterator;
        tmp = *cellIterator;
        // cout << "Cell iterator else if 1 = " << tmp->instName << endl;
      }
      //    if (step-start > timeLimit*0.9)
      //        iteration += circuitSize/20;
    } else {
      const long double expTemp =
          -initialTemp * (exp(-(double)(iteration) / (riseTime)) -
                          exp(-(double)(iteration) / (fallTime)));
      temp = expTemp
          //* (0.75-0.65*cos(((double)iteration/double(circuitSize*w5))))
          ;
      cont1 = (expTemp - finalTemp > 0.0) ? true : false;
      iteration += inc;
      // cout << "Temp else 1 = " << temp << endl;
    }

    if ((this->loadViol > 0.0) && (temp < 0.1) &&
        ((iteration2 <= 3) || (rand() % (10000 + circuitSize / 50) == 0))) {
      double lasLV = this->loadViol;
      inc = 0;
      do {
        ++inc;
        debug("tiago2", "load\t" << this->loadViol << "\t" << this->totalLeakage
                                 << "\t" << this->timingViol << " ("
                                 << this->minViol << ")"
                                 << "\t" << this->slewViol << "\t"
                                 << this->worstSlack << endl);
        lasLV = this->loadViol;
        // cout << "Temp if 2 " << temp << endl;
        this->changeCellsFastLoadSA(temp);
        if (iteration > offset2) iteration += 2000;
        debug("tiago2",
              "load\t\t" << this->loadViol << "\t" << this->totalLeakage << "\t"
                         << this->timingViol << " (" << this->minViol << ")"
                         << "\t" << this->slewViol << "\t" << this->worstSlack
                         << endl);
      } while (lasLV >
               this->loadViol * 1.01);  // solve max load violations (not
                                        // recursively -> can be changed)
    }

    if ((this->loadViol > 0.0) && (this->loadViol < 100.0) && (temp < 1e-3) &&
        ((iteration2 > offset2) || (iteration2 < offset1))) {
      ctrl = iteration2;
      double lasLV = this->loadViol;
      do {
        lasLV = this->loadViol;

        // cout << "Temp if 3 " << temp << endl;

        debug("tiago2",
              "LOAD_F\t" << this->loadViol << "\t" << this->totalLeakage << "\t"
                         << this->timingViol << " (" << this->minViol << ")"
                         << "\t" << this->slewViol << "\t" << this->worstSlack
                         << endl);
        this->solveLoadViol();
        debug("tiago2",
              "LOAD_F\t" << this->loadViol << "\t" << this->totalLeakage << "\t"
                         << this->timingViol << " (" << this->minViol << ")"
                         << "\t" << this->slewViol << "\t" << this->worstSlack
                         << endl);
        // debug("tiago2", " Temperature: " << temp << " Iteration #: " <<
        // iteration << "\t" << endl);
      } while (
          (this->loadViol > 0.0) &&
          (lasLV > this->loadViol * 1.01));  // solve max load violations (not
                                             // recursively -> can be changed)
      this->updateTiming();
      debug("tiago2", "LOAD_F\t" << this->loadViol << "\t" << this->totalLeakage
                                 << "\t" << this->timingViol << " ("
                                 << this->minViol << ")"
                                 << "\t" << this->slewViol << "\t"
                                 << this->worstSlack << endl);
    }

    if ((this->worstSlack <= -0.005) &&
        ((((iteration2 > offset2) || (iteration2 < offset1)) &&
          ((iteration - ctrl4 >
            100 +
                ((double)circuitSize * (1000.0 * temp) /
                 pow(max(1.0, -this->worstSlack / (myClock * 0.025)), 4.0))) ||
           (iteration2 - ctrl5 >
            100 + ((double)circuitSize * (1000.0 * temp) /
                   pow(max(1.0, -this->worstSlack / (myClock * 0.025)), 4.0))))
          //&& (temp < 0.25)
          && (this->timingViol / fabs(this->worstSlack) < circuitSize / 100.0)
          //&& (rand() % (circuitSize + \
											  min(int( (double) circuitSize * 10000 * (10*temp) * (10*temp) * (10*temp) * (10*temp) * (10*temp) / pow(max(10.0,this->timingViol),1.5) \
											  + 100000.0*circuitSize* \
											  ), circuitSize * 2)) == 0)))
          ))) {
      ctrl4 = iteration;
      ctrl5 = iteration2;
      // cout << "Temp if 4 " << temp << endl;
      const double piorSlack = this->worstSlack;
      inc = 0;
      /*
       if (iteration2 >= offset1) {
       cout << "\nSizingDepthFanoutXLogicalEffort..." << endl;
       sizingDepthFanoutXLogicalEffort();
       updateTiming();
       cout << "SizingDepthFanoutXLogicalEffort...done\n" << endl;
       }*/
      // for (int g = 0; g <= 0; ++g)
      {
        // debug("tiago2", endl << temp << endl << iteration << "\t" <<
        // iteration2 << "\tcritical\t" << this->loadViol << "\t" <<
        // this->totalLeakage << "\t" << this->timingViol << " (" <<
        // this->minViol << ")" << "\t" << this->slewViol << "\t" <<
        // this->worstSlack << endl);
        inc += this->changeCellsFastTimingSA(temp);
        // debug("tiago2", "\t\t\t" << this->loadViol << "\t" <<
        // this->totalLeakage << "\t" << this->timingViol << " (" <<
        // this->minViol << ")" << "\t" << this->slewViol << "\t" <<
        // this->worstSlack << endl); debug("tiago2", endl << temp << endl <<
        // iteration << "\t" << iteration2 << "\tcritical\t" << this->loadViol
        // << "\t" << this->totalLeakage << "\t" << this->timingViol << " (" <<
        // this->minViol << ")" << "\t" << this->slewViol << "\t" <<
        // this->worstSlack << endl);
      }
    } else {
      inc = this->changeCellsReimann(temp);
    } /*
if ((iteration2 < offset2) && (iteration2 > offset1)) {
if ((rejects+accepts > circuitSize*0.1) &&
((double)rejects/double(rejects+accepts) >= 0.99)) { debug("tiago2", "few
changes 5" << endl); iteration2 += circuitSize; rejects = accepts = 0;
}
}
if (iteration2 > offset2) {
if ((rejects+accepts > circuitSize*0.2) &&
((double)rejects/double(rejects+accepts) >= 0.999)) {
//debug("tiago2", "few changes 99.9\%" << endl);
iteration += circuitSize;
rejects = accepts = 0;
}
}*/
    /*
     if ((this->worstSlack <= -0.005) && (timingNumPathsWithNegativeSlack <
     numTails*0.01) && (iteration2 > offset2)) {
     sizingDepthFanoutXLogicalEffort();
     updateTiming();
     }*/

    if ((this->worstSlack > -0.005) && (this->loadViol <= 0.0)) {
      if ((this->totalLeakage < lastBestLeakage2 * 0.999)) {
        lastBestLeakage2 = this->totalLeakage;
        lastChange2 = iteration2;
      }
      if ((iteration2 - lastChange2 > (fewChangesCount)) ||
          ((rejects + accepts > circuitSize / 10) &&
           ((double)rejects / double(rejects + accepts) >= 0.99))) {
        debug("tiago2", "few changes 2" << endl);
        iteration2 += circuitSize / 10;
        lastBestLeakage2 = this->totalLeakage;
        lastChange2 = iteration2;
        rejects = accepts = 0;
      }

      if ((this->totalLeakage < lastBestLeakage * 0.999)) {
        lastBestLeakage = this->totalLeakage;
        lastChange = iteration;
      }
      if ((iteration - lastChange > (fewChangesCount)) ||
          ((rejects + accepts > circuitSize / 10) &&
           ((double)rejects / double(rejects + accepts) >= 0.97))) {
        debug("tiago2", "few changes 1" << endl);
        iteration += circuitSize / 10;
        lastBestLeakage = this->totalLeakage;
        lastChange = iteration;
        rejects = accepts = 0;
      }
    }
    if ((rejects + accepts) > circuitSize) rejects = accepts = 0;
    if (!cont)
      cout << temp << "\t" << finalTemp << "\t" << temp - finalTemp << endl;

    const int N = 100000;

    if (iteration - ctrl2 >= N) {
      tmp = *cellIterator;
      ctrl2 = iteration;
      printTimingDigest();
      debug("tiago2", this->loadViol
                          << "\t" << this->totalLeakage << "\t"
                          << this->timingViol << " (" << this->minViol << ")"
                          << "\t" << this->slewViol << "\t" << this->worstSlack
                          << endl);
      debug("tiago2", " Temperature: "
                          << temp << " Iteration #: " << iteration << "\t"
                          << iteration2 << "\t" << accepts << "\t" << rejects
                          << "\t"
                          << 100.0 * (double)rejects / double(rejects + accepts)
                          << "%\t" << tmp->logicalDepth
                          << endl);  //"\t" << cont1 << "\t" << cont2  << "\t"
                                     //<< (increasing) << "\t" << cont << endl);
    } else if ((iteration2 - ctrl3 >= N) &&
               (iteration2 < offset2 + circuitSize)) {
      ctrl3 = iteration2;
      tmp = *cellIterator;
      debug("tiago2", this->loadViol
                          << "\t" << this->totalLeakage << "\t"
                          << this->timingViol << " (" << this->minViol << ")"
                          << "\t" << this->slewViol << "\t" << this->worstSlack
                          << endl);
      debug("tiago2", " Temperature: "
                          << temp << " Iteration #: " << iteration << "\t"
                          << iteration2 << "\t" << accepts << "\t" << rejects
                          << "\t"
                          << 100.0 * (double)rejects / double(rejects + accepts)
                          << "%\t" << tmp->logicalDepth
                          << endl);  //"\t" << cont1 << "\t" << cont2  << "\t"
                                     //<< (increasing) << "\t" << cont << endl);
      printTimingDigest();
    }

    cont2 = (step - start < timeLimit * 0.99) ? true : false;
    cont = (cont1 && cont2) ? true : false;

    gettimeofday(&tp, NULL);
    step = static_cast<double>(tp.tv_sec);
  }

  debug("tiago2", " Temperature: " << temp << " Iteration #: " << iteration
                                   << "\t" << cont1 << "\t" << cont2 << "\t"
                                   << (increasing) << "\t" << cont << endl);

  if ((this->bestCost > this->totalLeakage) && (this->worstSlack > -0.005) &&
      (this->slewViol <= 0.0) && (this->loadViol <= 0.0)) {
    this->storeBestSolution();
    this->bestCost = this->totalLeakage;
    this->maxLeakage = this->totalLeakage;
  }

  gettimeofday(&tp, NULL);
  step = static_cast<double>(tp.tv_sec);

  cout << " Iterations: " << iteration << "\n Final Temperature: " << temp
       << "\t" << finalTemp << endl;
  cout << " Time (sec.): " << step - start << endl;

  cout << " Saving best solution found..." << endl;
  if (this->getBestSolution().size())
    this->saveBestSizes();
  else
    this->saveSizes();
  cout << " Saving best solution found...done" << endl;

  updateTimingLR();
  this->printTiming();

  cout << " Trocas aceitas " << acceptsChanges << endl;
  cout << " Trocas rejeitadas " << rejectsChanges << endl;

  cout << " Saving best solution found..." << endl;
  if (this->getBestSolution().size())
    this->saveBestSizes();
  else
    this->saveSizes();
  cout << " Saving best solution found...done" << endl;

  cout << "\nIterative sizing...done!\n" << endl;

  // printTrialsChanges();
}  // end method

// -----------------------------------------------------------------------------
void Circuit::lowTemperatureAnneal2() {
  const long int clo = clock();
  struct timeval tp;
  double start, step;

  // Time stamp before the computations
  gettimeofday(&tp, NULL);
  start = static_cast<double>(tp.tv_sec);
  // start = clock() / CLOCKS_PER_SEC;

  const long double minLeakage = this->totalLeakage;
  const double timeLimit =
      5 * 60 * 60 + 1 * 60 * 60 * ceil(this->getSize() / 35000.0);

  const int circuitSize = this->getSize();
  // const int iterationLimit = 10 * 10000000 + 2500 * circuitSize;
  // cout << " Iteration limit: " << iterationLimit << endl;

  this->maxTimingViol =
      this->getPathTailsSize() * (this->getClkPeriod() * 0.50) + 1000.0;
  this->maxWorstSlack = this->worstSlack;

  // double decay = 0.999;
  long double temp;
  const double finalTemp = 1.0e-4;
  // decay = 1.0-1.0/((100000+circuitSize)*20.0);
  double const initialTemp = 0.01;
  /*
   cout << " Initial temperature: " << temp << endl;
   cout << " Initial temperature decay: " << decay << endl;
   cout << " Final temperature: " << finalTemp << endl;
   */
  double lastBestLeakage = this->totalLeakage;
  double lastBestLeakage2 = this->totalLeakage;

  gettimeofday(&tp, NULL);
  step = static_cast<double>(tp.tv_sec);
  // step = clock() / CLOCKS_PER_SEC;
  /*
   this->solveLoadViol();
   this->updateTiming();
   */
  bool increasing = true;
  cout << "\nIterative sizing 1/1..." << endl;
  bool cont = true, cont1, cont2;
  const double fallTime = (circuitSize) * 15.0;
  const double riseTime = (circuitSize) * 1.0;

  const long unsigned int fewChangesCount = circuitSize / 100 + 1000;

  long unsigned int iteration = circuitSize / 10;

  int ctrl4 = iteration;
  int ctrl2 = iteration;
  int lastChange = iteration;

  const int numTails = timingTailNets.size();
  int inc = 0;

  const double myClock = this->getClkPeriod();
  this->maxTimingViol = (myClock * 0.01) * numTails;
  cout << " Maximum timing violation sum: " << maxTimingViol << endl;

  printTimingDigest(true, "Low Temperature SA");

  Vcell *tmp;
  while (cont) {
    cont1 = true;

    const long double expTemp =
        -initialTemp * (exp(-(double)(iteration) / (riseTime)) -
                        exp(-(double)(iteration) / (fallTime)));
    temp = expTemp
        //* (0.75-0.65*cos(((double)iteration/double(circuitSize*w5))))
        ;
    cont1 = (expTemp - finalTemp > 0.0) ? true : false;
    // cout << "Temp else 1 = " << temp << endl;
    inc = this->changeCellsReimann(temp);
    iteration += inc;

    // if ((this->worstSlack > -0.005) && (this->loadViol <= 0.0))
    {
      if ((this->totalLeakage < lastBestLeakage * 0.999)) {
        lastBestLeakage = this->totalLeakage;
        lastChange = iteration;
      }
      if ((iteration - lastChange > (fewChangesCount)) ||
          ((rejects + accepts > circuitSize / 10) &&
           ((double)rejects / double(rejects + accepts) >= 0.97))) {
        debug("tiago2", "few changes 1" << endl);
        iteration += circuitSize;
        lastBestLeakage = this->totalLeakage;
        lastChange = iteration;
        rejects = accepts = 0;
      }
    }
    if ((rejects + accepts) > circuitSize) rejects = accepts = 0;

    const int N = 10000;
    if (iteration - ctrl2 >= N) {
      tmp = *cellIterator;
      ctrl2 = iteration;
      printTimingDigest();
    }
    cont2 = (step - start < timeLimit * 0.99) ? true : false;

    cont = (cont1 && cont2) ? true : false;

    gettimeofday(&tp, NULL);
    step = static_cast<double>(tp.tv_sec);
  }

  debug("tiago2", " Temperature: " << temp << " Iteration #: " << iteration
                                   << "\t" << cont1 << "\t" << cont2 << "\t"
                                   << (increasing) << "\t" << cont << endl);

  if ((this->bestCost > this->totalLeakage) && (this->worstSlack > -0.005) &&
      (this->slewViol <= 0.0) && (this->loadViol <= 0.0)) {
    this->storeBestSolution();
    this->bestCost = this->totalLeakage;
    this->maxLeakage = this->totalLeakage;
  }

  gettimeofday(&tp, NULL);
  step = static_cast<double>(tp.tv_sec);

  cout << " Iterations: " << iteration << "\n Final Temperature: " << temp
       << "\t" << finalTemp << endl;
  cout << " Time (sec.): " << step - start << endl;

  cout << " Saving best solution found..." << endl;
  if (this->getBestSolution().size())
    this->saveBestSizes();
  else
    this->saveSizes();
  cout << " Saving best solution found...done" << endl;

  updateTimingLR();
  this->printTiming();

  cout << " Trocas aceitas " << acceptsChanges << endl;
  cout << " Trocas rejeitadas " << rejectsChanges << endl;

  cout << " Saving best solution found..." << endl;
  if (this->getBestSolution().size())
    this->saveBestSizes();
  else
    this->saveSizes();
  cout << " Saving best solution found...done" << endl;

  cout << "\nIterative sizing...done!\n" << endl;

  // printTrialsChanges();
}  // end method

// -----------------------------------------------------------------------------

void Circuit::lowTemperatureAnnealTestedWithUFSC() {
  const long int clo = clock();
  struct timeval tp;
  double start, step;

  // Time stamp before the computations
  gettimeofday(&tp, NULL);
  start = static_cast<double>(tp.tv_sec);
  // start = clock() / CLOCKS_PER_SEC;

  const long double minLeakage = this->totalLeakage;
  const double timeLimit =
      5 * 60 * 60 + 1 * 60 * 60 * ceil(this->getSize() / 35000.0);

  const int circuitSize = this->getSize();
  // const int iterationLimit = 10 * 10000000 + 2500 * circuitSize;
  // cout << " Iteration limit: " << iterationLimit << endl;

  this->maxTimingViol =
      this->getPathTailsSize() * (this->getClkPeriod() * 0.50) + 1000.0;
  this->maxWorstSlack = this->worstSlack;

  // double decay = 0.999;
  long double temp;
  const double finalTemp = 1.0e-4;
  // decay = 1.0-1.0/((100000+circuitSize)*20.0);
  double const initialTemp = 0.010;
  /*
   cout << " Initial temperature: " << temp << endl;
   cout << " Initial temperature decay: " << decay << endl;
   cout << " Final temperature: " << finalTemp << endl;
   */
  double lastBestLeakage = this->totalLeakage;
  double lastBestLeakage2 = this->totalLeakage;

  gettimeofday(&tp, NULL);
  step = static_cast<double>(tp.tv_sec);
  // step = clock() / CLOCKS_PER_SEC;
  /*
   this->solveLoadViol();
   this->updateTiming();
   */
  bool increasing = true;
  cout << "\nIterative sizing 1/1..." << endl;
  bool cont = true, cont1, cont2;
  const long unsigned int offset1 = 1;  // circuitSize * 10.0;
  const long unsigned int offset2 = circuitSize * 2.0 + offset1;
  const double fallTime = (circuitSize) * 15.0;
  const double riseTime = (circuitSize) * 1.0;

  const long unsigned int fewChangesCount = circuitSize / 100 + 1000;

  long unsigned int iteration = circuitSize;
  long unsigned int iteration2 = circuitSize;

  int ctrl = iteration2;
  int ctrl2 = iteration2;
  int ctrl3 = iteration2;
  int ctrl4 = iteration;
  int ctrl5 = iteration2;
  int lastChange = iteration;
  int lastChange2 = iteration2;

  const int numTails = timingTailNets.size();
  int inc = 0;

  const double myClock = this->getClkPeriod();
  this->maxTimingViol = (myClock * 0.01) * numTails;
  cout << " Maximum timing violation sum: " << maxTimingViol << endl;

  Vcell *tmp;
  while (cont) {
    cont1 = true;
    if (iteration2 < offset1 + circuitSize) {
      temp = 1.0e-5;
      iteration2 += inc;
      tmp = *cellIterator;
      while (tmp->logicalDepth <
             this->depthSortedCells.back()->logicalDepth / 2) {
        ++cellIterator;
        tmp = *cellIterator;
        // cout << "Cell iterator if 1 = " << tmp->instName << endl;
      }
      //    if (step-start > timeLimit*0.9)
      //       iteration += circuitSize/20;
    } else if (iteration2 < offset2 + circuitSize) {
      temp = 10.0e-4;
      iteration2 += inc;
      tmp = *cellIterator;
      while (tmp->logicalDepth <
             this->depthSortedCells.back()->logicalDepth / 4) {
        ++cellIterator;
        tmp = *cellIterator;
        // cout << "Cell iterator else if 1 = " << tmp->instName << endl;
      }
      //    if (step-start > timeLimit*0.9)
      //        iteration += circuitSize/20;
    } else {
      const long double expTemp =
          -initialTemp * (exp(-(double)(iteration) / (riseTime)) -
                          exp(-(double)(iteration) / (fallTime)));
      temp = expTemp
          //* (0.75-0.65*cos(((double)iteration/double(circuitSize*w5))))
          ;
      cont1 = (expTemp - finalTemp > 0.0) ? true : false;
      iteration += inc;
      // cout << "Temp else 1 = " << temp << endl;
    }

    if ((this->loadViol > 0.0) && (temp < 0.1) &&
        ((iteration2 <= 3) || (rand() % (10000 + circuitSize / 50) == 0))) {
      double lasLV = this->loadViol;
      inc = 0;
      do {
        ++inc;
        debug("tiago2", "load\t" << this->loadViol << "\t" << this->totalLeakage
                                 << "\t" << this->timingViol << " ("
                                 << this->minViol << ")"
                                 << "\t" << this->slewViol << "\t"
                                 << this->worstSlack << endl);
        lasLV = this->loadViol;
        // cout << "Temp if 2 " << temp << endl;
        this->changeCellsFastLoadSAUsedUFSC(temp);
        if (iteration > offset2) iteration += 2000;
        debug("tiago2",
              "load\t\t" << this->loadViol << "\t" << this->totalLeakage << "\t"
                         << this->timingViol << " (" << this->minViol << ")"
                         << "\t" << this->slewViol << "\t" << this->worstSlack
                         << endl);
      } while (lasLV >
               this->loadViol * 1.01);  // solve max load violations (not
                                        // recursively -> can be changed)
    }

    if ((this->loadViol > 0.0) && (this->loadViol < 100.0) && (temp < 1e-3) &&
        ((iteration2 > offset2) || (iteration2 < offset1))) {
      ctrl = iteration2;
      double lasLV = this->loadViol;
      do {
        lasLV = this->loadViol;

        // cout << "Temp if 3 " << temp << endl;

        debug("tiago2",
              "LOAD_F\t" << this->loadViol << "\t" << this->totalLeakage << "\t"
                         << this->timingViol << " (" << this->minViol << ")"
                         << "\t" << this->slewViol << "\t" << this->worstSlack
                         << endl);
        this->solveLoadViol();
        debug("tiago2",
              "LOAD_F\t" << this->loadViol << "\t" << this->totalLeakage << "\t"
                         << this->timingViol << " (" << this->minViol << ")"
                         << "\t" << this->slewViol << "\t" << this->worstSlack
                         << endl);
        // debug("tiago2", " Temperature: " << temp << " Iteration #: " <<
        // iteration << "\t" << endl);
      } while (
          (this->loadViol > 0.0) &&
          (lasLV > this->loadViol * 1.01));  // solve max load violations (not
                                             // recursively -> can be changed)
      this->updateTiming();
      debug("tiago2", "LOAD_F\t" << this->loadViol << "\t" << this->totalLeakage
                                 << "\t" << this->timingViol << " ("
                                 << this->minViol << ")"
                                 << "\t" << this->slewViol << "\t"
                                 << this->worstSlack << endl);
    }

    if ((this->worstSlack <= -0.005) &&
        ((((iteration2 > offset2) || (iteration2 < offset1)) &&
          ((iteration - ctrl4 >
            100 +
                ((double)circuitSize * (1000.0 * temp) /
                 pow(max(1.0, -this->worstSlack / (myClock * 0.025)), 4.0))) ||
           (iteration2 - ctrl5 >
            100 + ((double)circuitSize * (1000.0 * temp) /
                   pow(max(1.0, -this->worstSlack / (myClock * 0.025)), 4.0))))
          //&& (temp < 0.25)
          && (this->timingViol / fabs(this->worstSlack) < circuitSize / 100.0)
          //&& (rand() % (circuitSize + \
											   min(int( (double) circuitSize * 10000 * (10*temp) * (10*temp) * (10*temp) * (10*temp) * (10*temp) / pow(max(10.0,this->timingViol),1.5) \
											   + 100000.0*circuitSize* \
											   ), circuitSize * 2)) == 0)))
          ))) {
      ctrl4 = iteration;
      ctrl5 = iteration2;
      // cout << "Temp if 4 " << temp << endl;
      const double piorSlack = this->worstSlack;
      inc = 0;
      /*
       if (iteration2 >= offset1) {
       cout << "\nSizingDepthFanoutXLogicalEffort..." << endl;
       sizingDepthFanoutXLogicalEffort();
       updateTiming();
       cout << "SizingDepthFanoutXLogicalEffort...done\n" << endl;
       }*/
      // for (int g = 0; g <= 0; ++g)
      {
        // debug("tiago2", endl << temp << endl << iteration << "\t" <<
        // iteration2 << "\tcritical\t" << this->loadViol << "\t" <<
        // this->totalLeakage << "\t" << this->timingViol << " (" <<
        // this->minViol << ")" << "\t" << this->slewViol << "\t" <<
        // this->worstSlack << endl);
        inc += this->changeCellsFastTimingSA(temp);
        // debug("tiago2", "\t\t\t" << this->loadViol << "\t" <<
        // this->totalLeakage << "\t" << this->timingViol << " (" <<
        // this->minViol << ")" << "\t" << this->slewViol << "\t" <<
        // this->worstSlack << endl); debug("tiago2", endl << temp << endl <<
        // iteration << "\t" << iteration2 << "\tcritical\t" << this->loadViol
        // << "\t" << this->totalLeakage << "\t" << this->timingViol << " (" <<
        // this->minViol << ")" << "\t" << this->slewViol << "\t" <<
        // this->worstSlack << endl);
      }
    } else {
      inc = this->changeCellsReimann(temp);
    } /*
       if ((iteration2 < offset2) && (iteration2 > offset1)) {
       if ((rejects+accepts > circuitSize*0.1) &&
       ((double)rejects/double(rejects+accepts) >= 0.99)) { debug("tiago2", "few
       changes 5" << endl); iteration2 += circuitSize; rejects = accepts = 0;
       }
       }
       if (iteration2 > offset2) {
       if ((rejects+accepts > circuitSize*0.2) &&
       ((double)rejects/double(rejects+accepts) >= 0.999)) {
       //debug("tiago2", "few changes 99.9\%" << endl);
       iteration += circuitSize;
       rejects = accepts = 0;
       }
       }*/
    /*
     if ((this->worstSlack <= -0.005) && (timingNumPathsWithNegativeSlack <
     numTails*0.01) && (iteration2 > offset2)) {
     sizingDepthFanoutXLogicalEffort();
     updateTiming();
     }*/

    if ((this->worstSlack > -0.005) && (this->loadViol <= 0.0)) {
      if ((this->totalLeakage < lastBestLeakage2 * 0.999)) {
        lastBestLeakage2 = this->totalLeakage;
        lastChange2 = iteration2;
      }
      if ((iteration2 - lastChange2 > (fewChangesCount)) ||
          ((rejects + accepts > circuitSize / 10) &&
           ((double)rejects / double(rejects + accepts) >= 0.99))) {
        debug("tiago2", "few changes 2" << endl);
        iteration2 += circuitSize / 10;
        lastBestLeakage2 = this->totalLeakage;
        lastChange2 = iteration2;
        rejects = accepts = 0;
      }

      if ((this->totalLeakage < lastBestLeakage * 0.999)) {
        lastBestLeakage = this->totalLeakage;
        lastChange = iteration;
      }
      if ((iteration - lastChange > (fewChangesCount)) ||
          ((rejects + accepts > circuitSize / 10) &&
           ((double)rejects / double(rejects + accepts) >= 0.97))) {
        debug("tiago2", "few changes 1" << endl);
        iteration += circuitSize / 10;
        lastBestLeakage = this->totalLeakage;
        lastChange = iteration;
        rejects = accepts = 0;
      }
    }
    if ((rejects + accepts) > circuitSize) rejects = accepts = 0;
    if (!cont)
      cout << temp << "\t" << finalTemp << "\t" << temp - finalTemp << endl;

    if (iteration - ctrl2 >= 1000) {
      tmp = *cellIterator;
      ctrl2 = iteration;
      debug("tiago2", this->loadViol
                          << "\t" << this->totalLeakage << "\t"
                          << this->timingViol << " (" << this->minViol << ")"
                          << "\t" << this->slewViol << "\t" << this->worstSlack
                          << endl);
      debug("tiago2", " Temperature: "
                          << temp << " Iteration #: " << iteration << "\t"
                          << iteration2 << "\t" << accepts << "\t" << rejects
                          << "\t"
                          << 100.0 * (double)rejects / double(rejects + accepts)
                          << "%\t" << tmp->logicalDepth
                          << endl);  //"\t" << cont1 << "\t" << cont2  << "\t"
                                     //<< (increasing) << "\t" << cont << endl);
    } else if ((iteration2 - ctrl3 >= 1000) &&
               (iteration2 < offset2 + circuitSize)) {
      ctrl3 = iteration2;
      tmp = *cellIterator;
      debug("tiago2", this->loadViol
                          << "\t" << this->totalLeakage << "\t"
                          << this->timingViol << " (" << this->minViol << ")"
                          << "\t" << this->slewViol << "\t" << this->worstSlack
                          << endl);
      debug("tiago2", " Temperature: "
                          << temp << " Iteration #: " << iteration << "\t"
                          << iteration2 << "\t" << accepts << "\t" << rejects
                          << "\t"
                          << 100.0 * (double)rejects / double(rejects + accepts)
                          << "%\t" << tmp->logicalDepth
                          << endl);  //"\t" << cont1 << "\t" << cont2  << "\t"
                                     //<< (increasing) << "\t" << cont << endl);
    }

    cont2 = (step - start < timeLimit * 0.99) ? true : false;
    cont = (cont1 && cont2) ? true : false;

    gettimeofday(&tp, NULL);
    step = static_cast<double>(tp.tv_sec);
  }

  debug("tiago2", " Temperature: " << temp << " Iteration #: " << iteration
                                   << "\t" << cont1 << "\t" << cont2 << "\t"
                                   << (increasing) << "\t" << cont << endl);

  if ((this->bestCost > this->totalLeakage) && (this->worstSlack > -0.005) &&
      (this->slewViol <= 0.0) && (this->loadViol <= 0.0)) {
    this->storeBestSolution();
    this->bestCost = this->totalLeakage;
    this->maxLeakage = this->totalLeakage;
  }

  gettimeofday(&tp, NULL);
  step = static_cast<double>(tp.tv_sec);

  cout << " Iterations: " << iteration << "\n Final Temperature: " << temp
       << "\t" << finalTemp << endl;
  cout << " Time (sec.): " << step - start << endl;

  cout << " Saving best solution found..." << endl;
  if (this->getBestSolution().size())
    this->saveBestSizes();
  else
    this->saveSizes();
  cout << " Saving best solution found...done" << endl;

  updateTimingLR();
  this->printTiming();

  cout << " Trocas aceitas " << acceptsChanges << endl;
  cout << " Trocas rejeitadas " << rejectsChanges << endl;

  cout << " Saving best solution found..." << endl;
  if (this->getBestSolution().size())
    this->saveBestSizes();
  else
    this->saveSizes();
  cout << " Saving best solution found...done" << endl;

  cout << "\nIterative sizing...done!\n" << endl;
}  // end method

// -----------------------------------------------------------------------------

void Circuit::anneal() {
  const long int clo = clock();
  struct timeval tp;
  double start, step;

  // Time stamp before the computations
  gettimeofday(&tp, NULL);
  start = static_cast<double>(tp.tv_sec);
  // start = clock() / CLOCKS_PER_SEC;

  // computeCloudsReimann();

  const long double minLeakage = totalLeakage;
  const double timeLimit =
      5 * 60 * 60 + 1 * 60 * 60 * ceil(getSize() / 35000.0);

  const int circuitSize = getSize();
  // const int iterationLimit = 10 * 10000000 + 2500 * circuitSize;
  // cout << " Iteration limit: " << iterationLimit << endl;

  maxTimingViol = getPathTailsSize() * (getClkPeriod() * 0.50) + 1000.0;
  maxWorstSlack = worstSlack;

  // double decay = 0.999;

  gettimeofday(&tp, NULL);
  step = static_cast<double>(tp.tv_sec);
  // step = clock() / CLOCKS_PER_SEC;
  /*
   solveLoadViol();
   updateTiming();
   */
  int its = 1;
  double lastLeakage = DBL_MAX;
  while (lastLeakage > totalLeakage * 0.9 /*its <= 10*/) {
    lastLeakage = totalLeakage;
    long double temp;
    const double finalTemp = 1.0e-6;
    // decay = 1.0-1.0/((100000+circuitSize)*20.0);
    double const initialTemp = 1.0 + (0.5 * its);
    /*
     cout << " Initial temperature: " << temp << endl;
     cout << " Initial temperature decay: " << decay << endl;
     cout << " Final temperature: " << finalTemp << endl;
     */
    double lastBestLeakage = totalLeakage;
    double lastBestLeakage2 = totalLeakage;
    bool increasing = true;
    cout << "\nIterative sizing " << its << "..." << endl;
    bool cont = true, cont1, cont2;
    const long unsigned int offset1 = circuitSize * 1.0;
    const long unsigned int offset2 = circuitSize * 1.0 + offset1;
    const double fallTime = (circuitSize) * (1 + its);
    const double riseTime = (circuitSize) * 0.001;

    const long unsigned int fewChangesCount = circuitSize / 100 + 1000;

    long unsigned int iteration = circuitSize / 100;
    long unsigned int iteration2 = circuitSize;

    int ctrl = iteration2;
    int ctrl2 = iteration2;
    int ctrl3 = iteration2;
    int ctrl4 = iteration;
    int ctrl5 = iteration2;
    int lastChange = iteration;
    int lastChange2 = iteration2;

    const int numTails = timingTailNets.size();
    int inc = 0;

    const double myClock = getClkPeriod();
    maxTimingViol = (myClock * 0.01) * numTails;
    cout << " Maximum timing violation sum: " << maxTimingViol << endl;
    maxWorstSlack = -(myClock * 0.25) / its;
    cout << " Maximum worst Slack: " << maxWorstSlack << endl;

    Vcell *tmp;
    while (cont) {
      cont1 = true;
      if (iteration2 < offset1 + circuitSize) {
        temp = 1.5e-6;
        iteration2 += inc;
        tmp = *cellIterator;
        while (tmp->logicalDepth < depthSortedCells.back()->logicalDepth / 2) {
          ++cellIterator;
          tmp = *cellIterator;
        }
        //    if (step-start > timeLimit*0.9)
        //       iteration += circuitSize/20;
      } else if (iteration2 < offset2 + circuitSize) {
        temp = 10.0e-2;
        iteration2 += inc;
        tmp = *cellIterator;
        while (tmp->logicalDepth < depthSortedCells.back()->logicalDepth / 4) {
          ++cellIterator;
          tmp = *cellIterator;
        }
        //    if (step-start > timeLimit*0.9)
        //        iteration += circuitSize/20;
      } else {
        const long double expTemp =
            -initialTemp * (exp(-(double)(iteration) / (riseTime)) -
                            exp(-(double)(iteration) / (fallTime)));
        temp = expTemp
            //* (0.75-0.65*cos(((double)iteration/double(circuitSize*5))))
            ;
        cont1 = (expTemp - finalTemp > 0.0) ? true : false;
        iteration += inc;
      }

      if ((loadViol > 0.0) && (temp < 0.1) &&
          ((iteration2 <= 3) || (rand() % (10000 + circuitSize / 50) == 0))) {
        double lasLV = loadViol;
        inc = 0;
        do {
          ++inc;
          debug("tiago2", "load\t" << loadViol << "\t" << totalLeakage << "\t"
                                   << timingViol << " (" << minViol << ")"
                                   << "\t" << slewViol << "\t" << worstSlack
                                   << endl);
          lasLV = loadViol;
          changeCellsFastLoadSA(temp);
          if (iteration > offset2) iteration += 2000;
          debug("tiago2", "load\t\t" << loadViol << "\t" << totalLeakage << "\t"
                                     << timingViol << " (" << minViol << ")"
                                     << "\t" << slewViol << "\t" << worstSlack
                                     << endl);
        } while (lasLV > loadViol * 1.01);  // solve max load violations (not
                                            // recursively -> can be changed)
      }
      /*
       if ((loadViol > 0.0) && (loadViol < 100.0) && (temp < 1e-5) &&
       ((iteration2 > offset2) || (iteration2 < offset1))) { ctrl = iteration2;
       double lasLV = loadViol;
       do {
       lasLV = loadViol;
       debug("tiago2", "LOAD_F\t" << loadViol << "\t" << totalLeakage << "\t" <<
       timingViol << " (" << minViol << ")" << "\t" << slewViol << "\t" <<
       worstSlack << endl); solveLoadViol(); debug("tiago2", "LOAD_F\t" <<
       loadViol << "\t" << totalLeakage << "\t" << timingViol << " (" << minViol
       << ")" << "\t" << slewViol << "\t" << worstSlack << endl);
       //debug("tiago2", " Temperature: " << temp << " Iteration #: " <<
       iteration << "\t" << endl); } while ( (loadViol > 0.0) && (lasLV >
       loadViol * 1.01)); //solve max load violations (not recursively -> can be
       changed) updateTiming(); debug("tiago2", "LOAD_F\t" << loadViol << "\t"
       << totalLeakage << "\t" << timingViol << " (" << minViol << ")" << "\t"
       << slewViol << "\t" << worstSlack << endl);

       }
       */

      if ((worstSlack <= -0.005) &&
          ((((iteration2 > offset2) || (iteration2 < offset1)) &&
            ((iteration - ctrl4 >
              100 + ((double)circuitSize * (1000.0 * temp) /
                     pow(max(1.0, -worstSlack / (myClock * 0.025)), 4.0))) ||
             (iteration2 - ctrl5 >
              100 + ((double)circuitSize * (1000.0 * temp) /
                     pow(max(1.0, -worstSlack / (myClock * 0.025)), 4.0))))
            //&& (temp < 0.25)
            && (timingViol / fabs(worstSlack) < circuitSize / 100.0)
            //&& (rand() % (circuitSize + \
											min(int( (double) circuitSize * 10000 * (10*temp) * (10*temp) * (10*temp) * (10*temp) * (10*temp) / pow(max(10.0,timingViol),1.5) \
											+ 100000.0*circuitSize* \
											), circuitSize * 2)) == 0)))
            ))) {
        ctrl4 = iteration;
        ctrl5 = iteration2;
        const double piorSlack = worstSlack;
        inc = 0;
        /*
         if (iteration2 >= offset1) {
         cout << "\nSizingDepthFanoutXLogicalEffort..." << endl;
         sizingDepthFanoutXLogicalEffort();
         updateTiming();
         cout << "SizingDepthFanoutXLogicalEffort...done\n" << endl;
         }*/
        // for (int g = 0; g <= 0; ++g)
        {
          // debug("tiago2", endl << temp << endl << iteration << "\t" <<
          // iteration2 << "\tcritical\t" << loadViol << "\t" << totalLeakage <<
          // "\t" << timingViol << " (" << minViol << ")" << "\t" << slewViol <<
          // "\t" << worstSlack << endl);
          inc += changeCellsFastTimingSA(temp);
          // debug("tiago2", "\t\t\t" << loadViol << "\t" << totalLeakage <<
          // "\t" << timingViol << " (" << minViol << ")" << "\t" << slewViol <<
          // "\t" << worstSlack << endl); debug("tiago2", endl << temp << endl
          // << iteration << "\t" << iteration2 << "\tcritical\t" << loadViol <<
          // "\t" << totalLeakage << "\t" << timingViol << " (" << minViol <<
          // ")" << "\t" << slewViol << "\t" << worstSlack << endl);
        }
      } else {
        inc = changeCellsReimann(temp);
      }
      if ((iteration2 < offset2) && (iteration2 > offset1)) {
        if ((rejects + accepts > circuitSize * 0.1) &&
            ((double)rejects / double(rejects + accepts) >= 0.99)) {
          debug("tiago2", "few changes 5" << endl);
          iteration2 += circuitSize;
          rejects = accepts = 0;
        }
      }
      if (iteration2 > offset2) {
        if ((rejects + accepts > circuitSize * 0.2) &&
            ((double)rejects / double(rejects + accepts) >= 0.999)) {
          // debug("tiago2", "few changes 99.9\%" << endl);
          iteration += circuitSize;
          rejects = accepts = 0;
        }
      }
      /*
       if ((worstSlack <= -0.005) && (timingNumPathsWithNegativeSlack <
       numTails*0.01) && (iteration2 > offset2)) {
       sizingDepthFanoutXLogicalEffort();
       updateTiming();
       }*/

      if ((worstSlack > -0.005) && (loadViol <= 0.0)) {
        if ((totalLeakage < lastBestLeakage2 * 0.999)) {
          lastBestLeakage2 = totalLeakage;
          lastChange2 = iteration2;
        }
        if ((iteration2 - lastChange2 > (fewChangesCount)) ||
            ((rejects + accepts > 5000) &&
             ((double)rejects / double(rejects + accepts) >= 0.95))) {
          debug("tiago2", "few changes 2" << endl);
          iteration2 += circuitSize;
          lastBestLeakage2 = totalLeakage;
          lastChange2 = iteration2;
          rejects = accepts = 0;
        }

        if ((totalLeakage < lastBestLeakage * 0.999)) {
          lastBestLeakage = totalLeakage;
          lastChange = iteration;
        }
        if ((iteration - lastChange > (fewChangesCount)) ||
            ((rejects + accepts > 5000) &&
             ((double)rejects / double(rejects + accepts) >= 0.95))) {
          debug("tiago2", "few changes 1" << endl);
          iteration += circuitSize;
          lastBestLeakage = totalLeakage;
          lastChange = iteration;
          rejects = accepts = 0;
        }
      }
      if ((rejects + accepts) > circuitSize) rejects = accepts = 0;
      if (!cont)
        cout << temp << "\t" << finalTemp << "\t" << temp - finalTemp << endl;

      if (iteration - ctrl2 >= 1000) {
        tmp = *cellIterator;
        ctrl2 = iteration;
        debug("tiago2", loadViol << "\t" << totalLeakage << "\t" << timingViol
                                 << " (" << minViol << ")"
                                 << "\t" << slewViol << "\t" << worstSlack
                                 << endl);
        debug("tiago2",
              " Temperature: "
                  << temp << " Iteration #: " << iteration << "\t" << iteration2
                  << "\t" << accepts << "\t" << rejects << "\t"
                  << 100.0 * (double)rejects / double(rejects + accepts)
                  << "%\t" << tmp->logicalDepth
                  << endl);  //"\t" << cont1 << "\t" << cont2  << "\t" <<
                             //(increasing) << "\t" << cont << endl);
      } else if ((iteration2 - ctrl3 >= 1000) &&
                 (iteration2 < offset2 + circuitSize)) {
        ctrl3 = iteration2;
        tmp = *cellIterator;
        debug("tiago2", loadViol << "\t" << totalLeakage << "\t" << timingViol
                                 << " (" << minViol << ")"
                                 << "\t" << slewViol << "\t" << worstSlack
                                 << endl);
        debug("tiago2",
              " Temperature: "
                  << temp << " Iteration #: " << iteration << "\t" << iteration2
                  << "\t" << accepts << "\t" << rejects << "\t"
                  << 100.0 * (double)rejects / double(rejects + accepts)
                  << "%\t" << tmp->logicalDepth
                  << endl);  //"\t" << cont1 << "\t" << cont2  << "\t" <<
                             //(increasing) << "\t" << cont << endl);
      }

      cont2 = (step - start < timeLimit * 0.99) ? true : false;
      cont = (cont1 && cont2) ? true : false;

      gettimeofday(&tp, NULL);
      step = static_cast<double>(tp.tv_sec);

      if ((bestCost > totalLeakage) && (worstSlack > -0.005) &&
          (slewViol == 0.0) && (loadViol == 0.0)) {
        storeBestSolution();
        bestCost = totalLeakage;
        maxLeakage = totalLeakage;
      }
    }  // end while

    debug("tiago2", " Temperature: " << temp << " Iteration #: " << iteration
                                     << "\t" << cont1 << "\t" << cont2 << "\t"
                                     << (increasing) << "\t" << cont << endl);

    gettimeofday(&tp, NULL);
    step = static_cast<double>(tp.tv_sec);

    cout << " Iterations: " << iteration << "\n Final Temperature: " << temp
         << "\t" << finalTemp << endl;
    cout << " Time (sec.): " << step - start << endl;

    printTiming();
    ++its;
  }

  cout << " Saving best solution found..." << endl;
  if (getBestSolution().size())
    saveBestSizes();
  else
    saveSizes();
  cout << " Saving best solution found...done" << endl;

  updateTimingLR();
  printTiming();

  cout << " Saving best solution found..." << endl;
  if (getBestSolution().size())
    saveBestSizes();
  else
    saveSizes();
  cout << " Saving best solution found...done" << endl;

  cout << "\nIterative sizing...done!\n" << endl;

  // printTrialsChanges();
}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateTimingLR_PT() {
  // Combinational cells.
  /*
const int numNets = timingNets.size();
for ( int i = timingNumDummyNets; i < numNets; i++ )
updateTiming_Net(i);
*/
  // Find the worst arrival time.
  // updateTiming_WorstArrivalTime();

  updateRequiredTimeLR_PT();
  //[TODO LR]
  // update to respect KKT

}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateTimingLR_PT_KKT() {
  // Combinational cells.
  /*
const int numNets = timingNets.size();
for ( int i = timingNumDummyNets; i < numNets; i++ )
updateTiming_Net(i);
*/
  // Find the worst arrival time.
  // updateTiming_WorstArrivalTime();

  updateRequiredTimeLR_PT_KKT();
  //[TODO LR]
  // update to respect KKT

}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateTimingLR() {
  // Combinational cells.
  const int numNets = timingNets.size();
  for (int i = timingNumDummyNets; i < numNets; i++) updateTiming_Net(i);

  // Find the worst arrival time.
  updateTiming_WorstArrivalTime();

#ifndef NDEBUG
  updateTiming_Debug();
#endif

  updateRequiredTimeLR();

}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateTimingLR_KKT() {
  // Combinational cells.
  const int numNets = timingNets.size();
  for (int i = timingNumDummyNets; i < numNets; i++) updateTiming_Net(i);

  // Find the worst arrival time.
  updateTiming_WorstArrivalTime();
  updateTiming_SlewViolation();  // thread-safe slew violation update.
  updateTiming_Deprecated();     // keep old stuffs up-to-date.

#ifndef NDEBUG
  updateTiming_Debug();
#endif

  updateRequiredTimeLR_KKT();

}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateRequiredTimeLR() {
  const double T = sdcInfos.clk_period;
  const int numNets = timingNets.size();
  // cout << endl;
  double pk = 0.001 / (1.0 + kIndex / 10.0);
  // cout << "pk: " << pk << endl;

  // Set the required time for all nets to clock period.
  for (int i = 0; i < numNets; ++i) {
    timingRequiredTime[i].set(T, T);
    // timingNets[i].lambdaDelay.set(0.0, 0.0);
  }
  // Propagate back the required time.
  for (int i = numNets - 1; i >= timingNumDummyNets; --i) {
    const EdgeArray<double> &requiredTimeAtSink = timingRequiredTime[i];
    const TimingNet &net = timingNets[i];
    TimingNetState &netstate = getTimingNetState(i);

    EdgeArray<double> totalLambda(0.0, 0.0);
    // update required time and arc's lambdas
    const int k0 = timingArcPointers[i];
    const int k1 = timingArcPointers[i + 1];
    for (int k = k0; k < k1; k++) {
      const TimingArc &arc = timingArcs[k];
      TimingArcState &arcstate = getTimingArcState(k);

      EdgeArray<double> &requiredTimeAtDriver = timingRequiredTime[arc.driver];
      EdgeArray<double> slack;
      EdgeArray<double> lambda;
      for (int edge = 0; edge < 2; edge++) {
        const EdgeType reverseEdge = ((EdgeType)edge).getReversed();

        const double requiredTime =
            requiredTimeAtSink[edge] - arcstate.delay[edge];
        if (requiredTimeAtDriver[reverseEdge] > requiredTime)
          requiredTimeAtDriver[reverseEdge] = requiredTime;

        // Reimann: update slack at arcs using required timing in the output net
        // (sink) and arrival time in the driver net
        slack[edge] = getTimingNetState(arc.driver).arrivalTime[edge] +
                      arcstate.delay[reverseEdge] -
                      requiredTimeAtSink[reverseEdge];
        lambda[edge] =
            arcstate.lambda[edge] +
            pk * (slack[edge] - (netstate.arrivalTime[reverseEdge] -
                                 timingRequiredTime[i][reverseEdge]));
        lambda[edge] = max(0.0, lambda[edge]);
        totalLambda[edge] += lambda[edge];

        // cout << lambda[edge] << "\t" << -slack[edge] << "\t" <<
        // -(net.arrivalTime[reverseEdge]-timingRequiredTime[i][reverseEdge]) <<
        // endl;
      }  // end for
      arcstate.slack = slack;
      arcstate.lambda = lambda;
      // cout << arc.cell->instName << "/" << arc.cell->pinNetPairs[k-k0].first
      // << "\t slack neg.: " << arc.slack <<  "\t slews: " <<
      // timingNets[arc.driver].slew << "\tlambdas: " << arc.lambda << endl;
      // cout << arc.cell->instName << "/" << arc.cell->pinNetPairs[k-k0].first
      // << "\t" << net.lambdaDelay << "\tlambdas: " << arc.lambda << "\t" <<
      // arc.slack << endl;
    }  // end for

  }  // end for

}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateRequiredTimeLR_PT() {
  const double T = sdcInfos.clk_period;
  const int numNets = timingNets.size();

  double pk = 2.0 / (kIndex + 1.0);

  // Propagate back the required time.
  for (int i = numNets - 1; i >= timingNumDummyNets; --i) {
    const EdgeArray<double> &requiredTimeAtSink = timingRequiredTime[i];
    const TimingNet &net = timingNets[i];
    TimingNetState &netstate = getTimingNetState(i);

    EdgeArray<double> totalLambda(0.0, 0.0);
    EdgeArray<double> slackAtSink(DBL_MAX, DBL_MAX);

    // update arc's lambdas
    const int k0 = timingArcPointers[i];
    const int k1 = timingArcPointers[i + 1];
    for (int k = k0; k < k1; k++) {
      const TimingArc &arc = timingArcs[k];
      TimingArcState &arcstate = getTimingArcState(k);

      EdgeArray<double> &requiredTimeAtDriver = timingRequiredTime[arc.driver];
      EdgeArray<double> lambda;
      for (int edge = 0; edge < 2; edge++) {
        const EdgeType reverseEdge = ((EdgeType)edge).getReversed();
        slackAtSink[edge] = min(slackAtSink[edge], arcstate.slack[reverseEdge]);
      }  // end for

      for (int edge = 0; edge < 2; edge++) {
        const EdgeType reverseEdge = ((EdgeType)edge).getReversed();

        const double requiredTime =
            requiredTimeAtSink[edge] - arcstate.delay[edge];
        if (requiredTimeAtDriver[reverseEdge] > requiredTime)
          requiredTimeAtDriver[reverseEdge] = requiredTime;

        // Reimann: update lambda at arcs using slacks from PT
        lambda[edge] = arcstate.lambda[edge] +
                       pk * -(arcstate.slack[edge] - slackAtSink[reverseEdge]);
        lambda[edge] = max(0.0, lambda[edge]);

      }  // end for
      arcstate.lambda = lambda;
    }  // end for

  }  // end for
}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateRequiredTimeLR_KKT() {
  const double T = sdcInfos.clk_period;
  const int numNets = timingNets.size();
  // cout << endl;
  double pk = 0.01 / (1.0 + kIndex / 10.0);
  // cout << "pk: " << pk << " ";

  // Set the required time for all nets to clock period.
  for (int i = 0; i < numNets; ++i) {
    timingRequiredTime[i].set(T, T);
    getTimingNetState(i).lambdaDelay.set(0.0, 0.0);
  }
  int negSlack = 0;
  long double allLambda = 0.0;
  // Propagate back the required time.
  for (int i = numNets - 1; i >= timingNumDummyNets; --i) {
    const EdgeArray<double> &requiredTimeAtSink = timingRequiredTime[i];
    const TimingNet &net = timingNets[i];
    TimingNetState &netstate = getTimingNetState(i);

    EdgeArray<double> totalLambda(0.0, 0.0);
    // update required time and arc's lambdas
    const int k0 = timingArcPointers[i];
    const int k1 = timingArcPointers[i + 1];
    for (int k = k0; k < k1; k++) {
      const TimingArc &arc = timingArcs[k];
      TimingArcState &arcstate = getTimingArcState(k);

      EdgeArray<double> &requiredTimeAtDriver = timingRequiredTime[arc.driver];
      EdgeArray<double> slack;
      EdgeArray<double> lambda;
      for (int edge = 0; edge < 2; edge++) {
        const EdgeType reverseEdge = ((EdgeType)edge).getReversed();

        const double requiredTime =
            requiredTimeAtSink[edge] - arcstate.delay[edge];
        if (requiredTimeAtDriver[reverseEdge] > requiredTime)
          requiredTimeAtDriver[reverseEdge] = requiredTime;

        // Reimann: update slack at arcs using required timing in the output net
        // (sink) and arrival time in the driver net
        slack[edge] = getTimingNetState(arc.driver).arrivalTime[edge] +
                      arcstate.delay[reverseEdge] -
                      requiredTimeAtSink[reverseEdge];
        const int mu_v = (netstate.arrivalTime[reverseEdge] -
                          timingRequiredTime[i][reverseEdge]);

        if (slack[edge] > 0.0) {
          ++negSlack;
        }

        lambda[edge] = arcstate.lambda[edge] + pk * (/*slack[edge] -*/ mu_v);
        lambda[edge] = max(1.0e-12, lambda[edge]);
        totalLambda[edge] += lambda[edge];

        // cout << arc.lambda[edge] << "\t" << lambda[edge] << "\t" <<
        // -slack[edge] << "\t" <<
        // -(net.arrivalTime[reverseEdge]-timingRequiredTime[i][reverseEdge]) <<
        // endl;
      }  // end for
      arcstate.slack = slack;
      arcstate.lambda = lambda;
      // cout << net.driver->instName << "\t" << arc.lambda << "\t" << lambda <<
      // "\t" << slack << "\t" << (net.arrivalTime-timingRequiredTime[i]) <<
      // endl; cout << arc.cell->instName << "/" <<
      // arc.cell->pinNetPairs[k-k0].first << "\t slack neg.: " << arc.slack <<
      // "\t slews: " << timingNets[arc.driver].slew << "\tlambdas: " <<
      // arc.lambda << endl;
      // cout << arc.cell->instName << "/" << arc.cell->pinNetPairs[k-k0].first
      // << "\t" << net.lambdaDelay << "\tlambdas: " << arc.lambda << "\t" <<
      // arc.slack << endl;
    }  // end for

    // the critical trees should be known to update lambdas acoording to
    // endpoint slacks, otherwise its not possible keep KKT conditions??? cout
    // << net.driver->instName << "\t" << timingSinkNets[i] << "\t" <<
    // timingSinkNetPointers[i] << endl;

    // cout << i << "\t" << timingSinkNetPointers[i] << "\t" <<
    // timingSinkNetPointers[i + 1] << endl;
    if (timingSinkNetPointers[i] == timingSinkNetPointers[i + 1]) {
      // cout << "Only driving endpoint: " << net.driver->instName << endl;
      for (int k = k0; k < k1; k++) {
        const TimingArc &arc = timingArcs[k];
        TimingArcState &arcstate = getTimingArcState(k);

        EdgeArray<double> &requiredTimeAtDriver =
            timingRequiredTime[arc.driver];
        EdgeArray<double> lambda;
        for (int edge = 0; edge < 2; edge++) {
          const EdgeType reverseEdge = ((EdgeType)edge).getReversed();

          lambda[edge] =
              arcstate.lambda[edge] +
              pk *
                  ((netstate.arrivalTime[reverseEdge] -
                    timingRequiredTime
                        [i][reverseEdge]) /*/ max(totalLambda[edge],1.0e-12)*/);
          // cout << arc.lambda[edge] << " " <<  ((net.arrivalTime[reverseEdge]
          // - timingRequiredTime[i][reverseEdge])/* /
          // max(totalLambda[edge],1.0e-12)*/) << endl;
          lambda[edge] = max(1.0e-12, lambda[edge]);

          getTimingNetState(arc.driver).lambdaDelay[edge] += lambda[edge];
        }  // end for
        arcstate.lambda = lambda;
        allLambda += arcstate.lambda.aggregate();
        // cout << net.driver->instName << "\t" << arc.lambda << "\t" << lambda
        // << "\t" << totalLambda << "\t" <<
        // (net.arrivalTime-timingRequiredTime[i]) << endl; cout <<
        // arc.cell->instName << "/" << arc.cell->pinNetPairs[k-k0].first << "\t
        // slack neg.: " << arc.slack <<  "\t slews: " <<
        // timingNets[arc.driver].slew << "\tlambdas: " << arc.lambda << endl;
        // cout << arc.cell->instName << "/" <<
        // arc.cell->pinNetPairs[k-k0].first << "\t" << totalLambda <<
        // "\tlambdas: " << arc.lambda << "\t" << arc.slack << endl;
      }  // end for
    } else {
      for (int k = k0; k < k1; k++) {
        const TimingArc &arc = timingArcs[k];
        TimingArcState &arcstate = getTimingArcState(k);
        // cout << "NOT! only driving endpoint: " << net.driver->instName <<
        // "\tlambdas: " << totalLambda << endl;

        EdgeArray<double> &requiredTimeAtDriver =
            timingRequiredTime[arc.driver];
        EdgeArray<double> lambda;
        for (int edge = 0; edge < 2; edge++) {
          const EdgeType reverseEdge = ((EdgeType)edge).getReversed();
          lambda[edge] =
              arcstate.lambda[edge] * ((netstate.lambdaDelay[reverseEdge]) /
                                       max(totalLambda[edge], 1.0e-12));
          lambda[edge] = max(1.0e-12, lambda[edge]);
          getTimingNetState(arc.driver).lambdaDelay[edge] += lambda[edge];
        }  // end for
        // cout << net.driver->instName << "\t" << arc.lambda << "\t" << lambda
        // << "\t" << net.lambdaDelay << "\t" << totalLambda << endl;
        arcstate.lambda = lambda;
        allLambda += arcstate.lambda.aggregate();
        // cout << arc.cell->instName << "/" <<
        // arc.cell->pinNetPairs[k-k0].first << "\t slack neg.: " << arc.slack
        // <<  "\t slews: " << timingNets[arc.driver].slew << "\tlambdas: " <<
        // arc.lambda << endl; cout << arc.cell->instName << "/" <<
        // arc.cell->pinNetPairs[k-k0].first << "\t" << net.lambdaDelay <<
        // "\tlambdas: " << arc.lambda << "\t" << arc.slack << endl;

      }  // end for
    }
    // cout <<endl;
    const int outPin = net.driver->pinNetPairs.size() - 1;
    // cout << net.driver->instName << "/" <<
    // net.driver->pinNetPairs[outPin].first << "\t slack neg.: (" <<
    // -requiredTimeAtSink[RISE]+net.arrivalTime[RISE] << ", " <<
    // -requiredTimeAtSink[FALL]+net.arrivalTime[FALL] <<  ")\t slews: " <<
    // timingNets[i].slew << endl << endl;

  }  // end for
  cout << "# neg. slacks: " << negSlack << "\t"
       << "total lambda: " << allLambda << endl;
}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateRequiredTimeLR_PT_KKT() {
  const double T = sdcInfos.clk_period;
  const int numNets = timingNets.size();
  // cout << endl;
  double pk = (totalLeakage / referenceLeakage) / (kIndex + 1.0);
  // cout << " pk: " << pk << endl;

  // Set the required time for all nets to clock period.
  for (int i = 0; i < numNets; ++i) {
    // timingRequiredTime[i].set(T,T);
    getTimingNetState(i).lambdaDelay.set(0.0, 0.0);
  }
  // Propagate back the required time.
  for (int i = numNets - 1; i >= timingNumDummyNets; --i) {
    const EdgeArray<double> &requiredTimeAtSink = timingRequiredTime[i];
    const TimingNet &net = timingNets[i];
    TimingNetState &netstate = getTimingNetState(i);
    // cout << endl;
    EdgeArray<double> totalLambda(0.0, 0.0);
    EdgeArray<double> slackAtSink(DBL_MAX, DBL_MAX);
    // update arc's lambdas
    const int k0 = timingArcPointers[i];
    const int k1 = timingArcPointers[i + 1];
    for (int k = k0; k < k1; k++) {
      const TimingArc &arc = timingArcs[k];
      TimingArcState &arcstate = getTimingArcState(k);

      EdgeArray<double> &requiredTimeAtDriver = timingRequiredTime[arc.driver];
      EdgeArray<double> lambda;
      for (int edge = 0; edge < 2; edge++) {
        const EdgeType reverseEdge = ((EdgeType)edge).getReversed();
        slackAtSink[edge] = min(slackAtSink[edge], arcstate.slack[reverseEdge]);
      }  // end for

      for (int edge = 0; edge < 2; edge++) {
        const EdgeType reverseEdge = ((EdgeType)edge).getReversed();

        const double requiredTime =
            requiredTimeAtSink[edge] - arcstate.delay[edge];
        if (requiredTimeAtDriver[reverseEdge] > requiredTime)
          requiredTimeAtDriver[reverseEdge] = requiredTime;

        lambda[edge] = arcstate.lambda[edge] +
                       pk * -(arcstate.slack[edge] - slackAtSink[reverseEdge]);
        lambda[edge] = max(0.0, lambda[edge]);
        totalLambda[edge] += lambda[edge];

        // cout << arc.cell->instName << "/" <<
        // arc.cell->pinNetPairs[k-k0].first << "\t" << lambda[edge] << "\t" <<
        // arc.slack[edge] << "\t" << slackAtSink[reverseEdge] << endl;

      }  // end for
      // cout << arc.cell->instName << "/" << arc.cell->pinNetPairs[k-k0].first
      // << "\t" << arc.lambda[RISE] << "\t" << arc.lambda[FALL] << endl;
      arcstate.lambda = lambda;
      // cout << arc.cell->instName << "/" << arc.cell->pinNetPairs[k-k0].first
      // << "\t" << arc.lambda[RISE] << "\t" << arc.lambda[FALL] << endl;
    }  // end for

    // cout << net.driver->instName << "\t" << timingSinkNets[i] << "\t" <<
    // timingSinkNetPointers[i] << endl; cout << "slack at sink: " <<
    // slackAtSink[RISE] << "\t" << slackAtSink[FALL] << endl;

    if (timingSinkNetPointers[i] == timingSinkNetPointers[i + 1]) {
      // cout << "Only driving endpoint: " << net.driver->instName << endl;
      for (int k = k0; k < k1; k++) {
        const TimingArc &arc = timingArcs[k];
        TimingArcState &arcstate = getTimingArcState(k);

        EdgeArray<double> &requiredTimeAtDriver =
            timingRequiredTime[arc.driver];
        EdgeArray<double> lambda;
        for (int edge = 0; edge < 2; edge++) {
          const EdgeType reverseEdge = ((EdgeType)edge).getReversed();

          lambda[edge] =
              arcstate.lambda[edge] +
              pk * arcstate.lambda[edge] *
                  (-slackAtSink[reverseEdge] / max(totalLambda[edge], 1.0e-12));
          lambda[edge] = max(0.0, lambda[edge]);
          getTimingNetState(arc.driver).lambdaDelay[edge] += lambda[edge];
        }  // end for

        arcstate.lambda = lambda;

        // cout << arc.cell->instName << "/" <<
        // arc.cell->pinNetPairs[k-k0].first << "\t" << arc.slack[RISE] << "\t"
        // << arc.slack[FALL] <<  "\t" << timingNets[arc.driver].slew[RISE] <<
        // "\t" << timingNets[arc.driver].slew[FALL] << "\tlambdas: " <<
        // arc.lambda[RISE] << "\t" << arc.lambda[FALL] << endl << endl; cout <<
        // arc.cell->instName << "/" << arc.cell->pinNetPairs[k-k0].first <<
        // "\t" << totalLambda << "\tlambdas: " << arc.lambda << "\t" <<
        // arc.slack << endl;

      }  // end for
    } else {
      // cout << "NOT! only driving endpoint: " << net.driver->instName << endl;
      for (int k = k0; k < k1; k++) {
        const TimingArc &arc = timingArcs[k];
        TimingArcState &arcstate = getTimingArcState(k);

        EdgeArray<double> lambda;
        for (int edge = 0; edge < 2; edge++) {
          const EdgeType reverseEdge = ((EdgeType)edge).getReversed();

          // cout << "netlambdadelay: " << net.lambdaDelay << "\t" <<
          // totalLambda << endl;

          lambda[edge] =
              arcstate.lambda[edge] * ((netstate.lambdaDelay[reverseEdge]) /
                                       max(totalLambda[edge], 1.0e-12));
          lambda[edge] = max(0.0, lambda[edge]);
          getTimingNetState(arc.driver).lambdaDelay[edge] += lambda[edge];
        }  // end for
        // cout << arc.cell->instName << "/" <<
        // arc.cell->pinNetPairs[k-k0].first << "\t" << arc.slack[RISE] << "\t"
        // << arc.slack[FALL] <<  "\t" << timingNets[arc.driver].slew[RISE] <<
        // "\t" << timingNets[arc.driver].slew[FALL] << "\tlambdas: " <<
        // arc.lambda[RISE] << "\t" << arc.lambda[FALL] << endl;
        arcstate.lambda = lambda;
        // cout << arc.cell->instName << "/" <<
        // arc.cell->pinNetPairs[k-k0].first << "\t" << arc.slack[RISE] << "\t"
        // << arc.slack[FALL] <<  "\t" << timingNets[arc.driver].slew[RISE] <<
        // "\t" << timingNets[arc.driver].slew[FALL] << "\tlambdas: " <<
        // arc.lambda[RISE] << "\t" << arc.lambda[FALL] << endl; cout <<
        // arc.cell->instName << "/" << arc.cell->pinNetPairs[k-k0].first <<
        // "\t" << net.lambdaDelay << "\tlambdas: " << arc.lambda << "\t" <<
        // arc.slack << endl;

      }  // end for
    }
    const int outPin = net.driver->pinNetPairs.size() - 1;
  }  // end for
}  // end method

// -----------------------------------------------------------------------------

void Circuit::readTimingLR() {
  // read timing from PrimeTime
  // store values on cells
  // shouldn't be used

  Vcell *tmpCell = new Vcell;
  AddrCell tmpCell1;
  set<AddrCell>::iterator it;
  int index = 0, lastViol = 0;
  // char nada;
  string filename = rootDir + "/" + benchName + "/" + benchName + ".timing";

  map<string, int> cellNameMap;
  map<string, int>::iterator cellIndex;
  const int cellCount = depthSortedCells.size();
  for (int i = offsetSequential; i < cellCount; ++i) {
    cellNameMap.insert(make_pair(depthSortedCells[i]->instName, i));
  }

  worstSlack = DBL_MAX;
  double totalSlack = 0.0;
  worstSlew = 0.0;

  hasViol = false;

  lastViol = timingViol;
  timingViol = 0;

  if (!fopen(filename.c_str(), "r")) return;
  TimingParser tp(filename);
  cout << " Reading timing from PrimeTime..." << endl;
  bool valid = false;
  do {
    string cellInst, pin;
    double riseSlack, fallSlack, riseTransition, fallTransition;
    valid = tp.read_pin_timing(cellInst, pin, riseSlack, fallSlack,
                               riseTransition, fallTransition);

    if (valid) {
      //	std::cout << " Timing for pin: " << cellInst << "/" << pin << ":
      //" << riseSlack << " " << fallSlack << " " << riseTransition << " " <<
      // fallTransition << endl ;

      cellIndex = cellNameMap.find(cellInst);

      Vcell *cell = depthSortedCells[cellIndex->second];

      // const int c = cell->depthIndex;
      const int n = cell->sinkNetIndex;

      // const TimingNet &net = timingNets[n];
      const int k0 = timingArcPointers[n];
      const int k1 = timingArcPointers[n + 1];
      for (int k = k0; k < k1; k++) {
        if (timingArcDriverPinName[k] == pin) {
          TimingArcState &arcstate = getTimingArcState(k);

          arcstate.slack.set(riseSlack, fallSlack);
          arcstate.oslew.set(riseTransition, fallTransition);
          break;
        }  // end if
      }    // end for

      worstSlack = min(worstSlack, min(riseSlack, fallSlack));
      worstSlew = max(worstSlew, max(riseTransition, fallTransition));
    }

  } while (valid);

  do {
    string port;
    double riseSlack, fallSlack, riseTransition, fallTransition;

    valid = tp.read_port_timing(port, riseSlack, fallSlack, riseTransition,
                                fallTransition);

    if (valid) {
      // s	std::cout << " Timing for port: " << port << ": " << riseSlack
      // << " " << fallSlack << " " << riseTransition << " " << fallTransition
      // << endl ;
    }

  } while (valid);

  cout << "  Worst slack found: " << worstSlack << endl;
  cout << "  Worst slew found: " << worstSlew << " (" << minSlew << ")" << endl;
  // cout << "  Total negative slack: " << totalSlack << endl;
  cout << " Reading timing from PrimeTime...done" << endl;
  minSlew = min(minSlew, worstSlew);
  minViol = min(minViol, timingViol);
}

// -----------------------------------------------------------------------------

void Circuit::runDP() {
  updateTiming();
  updateRequiredTime();
  const double loadViolPenalty = 1000.0;
  const double originalLoadViolation = loadViol;
  const double originalSlewViolation = slewViol;

  // cleaning up dummy nets costs (shouldn't be necessary)
  for (int i = 0; i < timingNumDummyNets; ++i) {
    Vcell *driver = timingNets[timingSinkNets[i]].driver;
    const int libOptions = orgCells.oCells[driver->footprintIndex].cells.size();
    const int costVectorIndex = lagrangianMapPointers[timingSinkNets[i]];
    setDeltaLoadSlew(i);
    driver->treeIndex = -1;
    for (int a = 0; a < libOptions; ++a) {
      lagrangianMap[costVectorIndex][a] = 0.0;
    }
  }

  const int numNets = timingNets.size();
  // set subnode weight
  // debug("tiago10", "Calculating subnodes costs.");
  for (int i = timingNumDummyNets; i < numNets; ++i) {
    Vcell *driver = timingNets[i].driver;

    driver->treeIndex = -1;
    setDeltaLoadSlew(i);
    const double originalSizingEffect = computeSizingEffectOnDriverCellDelay(i);
    const double originalLeakage = driver->actualInstType->leakagePower;

    const int costVectorIndex = lagrangianMapPointers[i];
    const int lastOption = driver->actualInstTypeIndex;
    const int libOptions = orgCells.oCells[driver->footprintIndex].cells.size();

    if (driver->dontTouch) {
      lagrangianMap[costVectorIndex][lastOption] =
          originalLeakage + originalSizingEffect;
      // debug("tiago10", "\tCell: " << driver->instName << "(" <<
      // driver->instType << ")\tcost: " <<
      // lagrangianMap[costVectorIndex][lastOption] << endl;);
    } else {
      for (int a = 0; a < libOptions; ++a) {
        updateCellType(driver, a);
        const double costSizingEffectOnDelay =
            computeSizingEffectOnDriverCellDelay(
                i);  // sum(all arcs)(lambda * delay)

        const double cost =
            orgCells.oCells[driver->footprintIndex].cells[a].leakagePower +
            costSizingEffectOnDelay;
        lagrangianMap[costVectorIndex][a] = cost;
        // debug("tiago10", "\tCell: " << driver->instName << "(" <<
        // driver->instType << ")\tcost: " << lagrangianMap[costVectorIndex][a]
        // << "\tfor: " << a << endl;);
      }
      updateCellType(driver, lastOption);
      updateTimingDriverCell(i);
    }
  }
  // calculate edges weight, choose best option and propagate accumulated cost
  for (int i = timingOffsetToLevelOneNets; i < numNets; ++i) {
    Vcell *driver = timingNets[i].driver;

    vector<EdgeArray<double>> originalDelays;

    const int costVectorIndex = lagrangianMapPointers[i];
    const int driverLastOption = driver->actualInstTypeIndex;

    const int libOptions = orgCells.oCells[driver->footprintIndex].cells.size();
    for (int a = 0; a < libOptions; ++a) {
      // compute cost for all previous cells (timingArcs)
      const int k0 = timingArcPointers[i];
      const int k1 = timingArcPointers[i + 1];
      for (int k = k0; k < k1; k++) {
        TimingArc &arc = timingArcs[k];
        TimingArcState &arcstate = getTimingArcState(k);

        Vcell *arcDriver = timingNets[arc.driver].driver;
        const int arcDriverCostVectorIndex = lagrangianMapPointers[arc.driver];
        double totalArcLambda = 0.0, maxArcLambda = 0.0,
               worstArcSlack = DBL_MAX;
        const int arcOutputPin = arcDriver->returnPinIndex("o");
        const int driverOutputPin = driver->returnPinIndex("o");

        const bool ignoredArc = ignoreArcOzdal(k, arcstate);

        const int arcDriverLastOption = arcDriver->actualInstTypeIndex;

        int pin = arc.pin;
        const double deltaLoad = orgCells.oCells[driver->footprintIndex]
                                     .cells[a]
                                     .pins[pin]
                                     .capacitance -
                                 orgCells.oCells[driver->footprintIndex]
                                     .cells[driverLastOption]
                                     .pins[pin]
                                     .capacitance;

        int arcDriverBestOption = -1;
        double arcDriverBestCost = DBL_MAX;

        if ((arcDriver->dontTouch)) {  // arc driven by sequential cell or input
                                       // driver

          const double arcDriverMaxLoad =
              orgCells.oCells[arcDriver->footprintIndex]
                  .cells[arcDriverLastOption]
                  .pins[arcOutputPin]
                  .maxCapacitance;
          const double driverMaxLoad = orgCells.oCells[driver->footprintIndex]
                                           .cells[arcDriverLastOption]
                                           .pins[driverOutputPin]
                                           .maxCapacitance;
          const double addedLoadViol =
              max(0.0,
                  ((arcDriver->actualLoad + deltaLoad) - arcDriverMaxLoad)) +
              max(0.0, driver->actualLoad - driverMaxLoad);

          const double sizingEffectOnSlew = deltaLoad * slewImpact(arc);

          const double arcDriverSizingEffectOnDelay =
              deltaLoad * delayImpact(arcDriver->sinkNetIndex);

          const double arcDriverCost =
              arcDriverSizingEffectOnDelay + sizingEffectOnSlew +
              max(loadViolPenalty * (addedLoadViol), 0.0) +
              lagrangianMap[arcDriverCostVectorIndex][arcDriverLastOption];

          arcDriverBestOption = arcDriverLastOption;
          arcDriverBestCost = arcDriverCost;

        } else {
          const int arcDriverLibOptions =
              orgCells.oCells[arcDriver->footprintIndex].cells.size();
          if (ignoredArc) {  // arc driven by cell not belonging to this tree

            const double arcDriverMaxLoad =
                orgCells.oCells[arcDriver->footprintIndex]
                    .cells[arcDriverLastOption]
                    .pins[arcOutputPin]
                    .maxCapacitance;
            const double driverMaxLoad = orgCells.oCells[driver->footprintIndex]
                                             .cells[a]
                                             .pins[driverOutputPin]
                                             .maxCapacitance;
            const double addedLoadViol =
                max(0.0,
                    ((arcDriver->actualLoad + deltaLoad) - arcDriverMaxLoad)) +
                max(0.0, driver->actualLoad - driverMaxLoad);

            const double sizingEffectOnSlew = deltaLoad * slewImpact(arc);

            const double arcDriverSizingEffectOnDelay =
                deltaLoad * delayImpact(arcDriver->sinkNetIndex);

            const double arcDriverCost =
                arcDriverSizingEffectOnDelay + sizingEffectOnSlew +
                max(loadViolPenalty * (addedLoadViol), 0.0)
                //+ lagrangianMap[arcDriverCostVectorIndex][arcDriverLastOption]
                ;

            arcDriverBestOption = -1;
            arcDriverBestCost = arcDriverCost;

          } else {  // arc driven by cell belonging to this tree

            arcDriverBestOption = -1;
            arcDriverBestCost = DBL_MAX;
            const double driverMaxLoad = orgCells.oCells[driver->footprintIndex]
                                             .cells[a]
                                             .pins[driverOutputPin]
                                             .maxCapacitance;
            for (int p = 0; p < arcDriverLibOptions; ++p) {
              const double arcDriverMaxLoad =
                  orgCells.oCells[arcDriver->footprintIndex]
                      .cells[p]
                      .pins[arcOutputPin]
                      .maxCapacitance;
              const double addedLoadViol =
                  max(0.0, ((arcDriver->actualLoad + deltaLoad) -
                            arcDriverMaxLoad)) +
                  max(0.0, driver->actualLoad - driverMaxLoad);

              const double sizingEffectOnSlew = deltaLoad * slewImpact(arc);

              const double arcDriverSizingEffectOnDelay =
                  deltaLoad * delayImpact(arcDriver->sinkNetIndex);

              const double arcDriverCost =
                  arcDriverSizingEffectOnDelay + sizingEffectOnSlew +
                  max(loadViolPenalty * (addedLoadViol), 0.0) +
                  lagrangianMap[arcDriverCostVectorIndex][p];

              if (arcDriverCost < arcDriverBestCost) {
                arcDriverBestOption = p;
                arcDriverBestCost = arcDriverCost;
              }  // end if
            }    // end for
          }      // end else
        }        // end else
        if (arcDriverBestOption > -1) {
          lagrangianMap[costVectorIndex][a] +=
              lagrangianMap[arcDriverCostVectorIndex][arcDriverBestOption] +
              arcDriverBestCost;
        }
        lagrangianMap[costVectorIndex + 1 + (k - k0)][a] = arcDriverBestOption;

      }  // end for (arcs)
    }    // end for (libOptions)
         /*
         if (isRoot(i)) {
             backTrack(i);
         }*/
  }      // end for (timingNets)

  int ignored = 0;
  // backtrack sizing cells
  for (int i = timingNets.size() - 1; i >= timingOffsetToLevelOneNets; --i) {
    Vcell *driver = timingNets[i].driver;

    int bestOption = -1;
    double bestCost = DBL_MAX;
    const int costVectorIndex = lagrangianMapPointers[i];

    // find best option
    const int libOptions = orgCells.oCells[driver->footprintIndex].cells.size();
    for (int a = 0; a < libOptions; ++a) {
      if (lagrangianMap[costVectorIndex][a] < bestCost) {
        bestOption = a;
        bestCost = lagrangianMap[costVectorIndex][a];
      }  // end if
    }    // end for

    updateCellType(driver, bestOption);

    // propagate back and evaluate tree extraction possibilities
    const int k0 = timingArcPointers[i];
    const int k1 = timingArcPointers[i + 1];
    for (int k = k0; k < k1; k++) {
      TimingArc &arc = timingArcs[k];
      TimingArcState &arcstate = getTimingArcState(k);

      Vcell *arcDriver = timingNets[arc.driver].driver;

      const bool ignoredArc = ignoreArc(arc, arcstate);

      // propagate back decision
      const int arcDriverCostVectorIndex = lagrangianMapPointers[arc.driver];
      if ((!arcDriver->dontTouch) && (!ignoredArc)) {
        lagrangianMap[arcDriverCostVectorIndex]
                     [lagrangianMap[costVectorIndex + 1 + k - k0][bestOption]] =
                         -DBL_MAX;
      }  // end if
    }    // end for
  }      // end for

  updateTiming();

}  // end method

// -----------------------------------------------------------------------------

void Circuit::criticalTreeExtraction() {
  updateTiming();
  updateRequiredTime();

  // cleaning up dummy nets costs (shouldn't be necessary)
  for (int i = 0; i < timingNumDummyNets; ++i) {
    Vcell *driver = timingNets[timingSinkNets[i]].driver;
    const int libOptions = orgCells.oCells[driver->footprintIndex].cells.size();
    const int costVectorIndex = lagrangianMapPointers[timingSinkNets[i]];
    setDeltaLoadSlew(i);
    driver->treeIndex = -1;
    for (int a = 0; a < libOptions; ++a) {
      lagrangianMap[costVectorIndex][a] = 0.0;
    }
  }

  const int numNets = timingNets.size();
  // set subnode weight
  for (int i = timingNumDummyNets; i < numNets; ++i) {
    Vcell *driver = timingNets[i].driver;

    setDeltaLoadSlew(i);
    driver->treeIndex = -1;

    const int costVectorIndex = lagrangianMapPointers[i];
    const int lastOption = driver->actualInstTypeIndex;
    const int libOptions = orgCells.oCells[driver->footprintIndex].cells.size();

    if (driver->dontTouch) {
      const double originalSizingEffect =
          computeSizingEffectOnDriverCellDelay(i);
      const double originalLeakage = driver->actualInstType->leakagePower;
      lagrangianMap[costVectorIndex][lastOption] =
          originalLeakage + originalSizingEffect;
    } else {
      for (int a = 0; a < libOptions; ++a) {
        updateCellType(driver, a);
        const double costSizingEffectOnDelay =
            computeSizingEffectOnDriverCellDelay(
                i);  // sum(all arcs)(lambda * delay)

        const double cost =
            orgCells.oCells[driver->footprintIndex].cells[a].leakagePower +
            costSizingEffectOnDelay
            //+ (loadViolPenalty*(loadViol - originalLoadViolation))
            ;
        lagrangianMap[costVectorIndex][a] = cost;
      }
      updateCellType(driver, lastOption);
      updateTimingDriverCell(i);
    }
  }

  // find roots
  for (int i = timingOffsetToLevelOneNets; i < numNets; ++i) {
    if (isRoot(i)) backTrack(i);
  }  // end for (timingNets)

  updateTiming();

}  // end method

// -----------------------------------------------------------------------------

double Circuit::slewImpact(TimingArc &arc) {  //

  Vcell *arcDriver = timingNets[arc.driver].driver;

  double sizingEffectOnSlew = 0.0;
  const int arc0 = timingSinkArcPointers[arcDriver->sinkNetIndex];
  const int arc1 = timingSinkArcPointers[arcDriver->sinkNetIndex + 1];
  for (int arcIndex = arc0; arcIndex < arc1; ++arcIndex) {
    const TimingArc &arc2 = timingArcs[timingSinkArcs[arcIndex]];
    if ((!arc2.cell) || (arc2.cell == arc.cell)) continue;
    const TimingArcState &arcstate2 =
        getTimingArcState(timingSinkArcs[arcIndex]);

    const EdgeArray<double> slewVariationOnDriver =
        deltaSlew_deltaLoad[arcDriver->sinkNetIndex];
    const EdgeArray<double> delayVariationOnSink =
        deltaDelay_deltaSlew[arc2.cell->sinkNetIndex];

    sizingEffectOnSlew += (arcstate2.lambda.getReversed() *
                           slewVariationOnDriver * delayVariationOnSink)
                              .getMax();
  }
  return sizingEffectOnSlew;
}  // end method

// -----------------------------------------------------------------------------

double Circuit::delayImpact(const int netIndex) {  //

  EdgeArray<double> impact(0.0, 0.0);

  const int k0 = timingArcPointers[netIndex];
  const int k1 = timingArcPointers[netIndex + 1];
  for (int k = k0; k < k1; k++) {
    // Update arc timings.
    const TimingArcState &arcstate = getTimingArcState(k);

    impact += deltaDelay_deltaLoad[k] * arcstate.lambda;
  }  // end for

  return impact.getMax();
}  // end method

// -----------------------------------------------------------------------------

bool Circuit::isRoot(const int netIndex) {  //

  bool root = true;
  /* if (timingNets[netIndex].fanout == 1) {
       Vcell * driver = timingNets[netIndex].driver;
       cout << "1 fanout: " << driver->instName << endl;
   }*/
  const int a0 = timingSinkArcPointers[netIndex];
  const int a1 = timingSinkArcPointers[netIndex + 1];
  for (int a = a0; a < a1; ++a) {
    TimingArc &arc = timingArcs[timingSinkArcs[a]];
    TimingArcState &arcstate = getTimingArcState(timingSinkArcs[a]);
    Vcell *arcDriver = timingNets[arc.driver].driver;

    assert(timingNets[netIndex].driver == timingNets[arc.driver].driver);
    if (!arc.cell) continue;
    if (!ignoreArcOzdal(timingSinkArcs[a], arcstate)) return false;
  }

  // cout << "root: " << netIndex << endl;
  return true;

}  // end method

// -----------------------------------------------------------------------------

void Circuit::backTrack(const int netIndex) {
  // [WARNING] static is not thread-safe :)
  static int treeIndex = 0;
  static int changed = 0;
  const double loadViolPenalty = 1000000.0;

  Vcell *rootCell = timingNets[netIndex].driver;
  const double alpha = 1.0e3;

  ++treeIndex;
  queue<Vcell *> q;
  stack<Vcell *> s;
  q.push(rootCell);
  s.push(rootCell);

  while (!q.empty()) {
    Vcell *driver = q.front();
    q.pop();

    assert(driver->treeIndex == -1);

    driver->treeIndex = treeIndex;

    if (driver->dontTouch) {
      continue;
    }

    const int i = driver->sinkNetIndex;

    updateTimingDriverCell(i);
    setDeltaLoadSlew(i);
    const double originalSizingEffect = computeSizingEffectOnDriverCellDelay(i);
    const double originalLeakage = driver->actualInstType->leakagePower;

    const int costVectorIndex = lagrangianMapPointers[i];
    const int lastOption = driver->actualInstTypeIndex;
    const int libOptions = orgCells.oCells[driver->footprintIndex].cells.size();

    if (driver->dontTouch) {
      lagrangianMap[costVectorIndex][lastOption] =
          alpha * originalLeakage + originalSizingEffect;
      // debug("tiago10", "\tCell: " << driver->instName << "(" <<
      // driver->instType << ")\tcost: " <<
      // lagrangianMap[costVectorIndex][lastOption] << endl;);
    } else {
      for (int a = 0; a < libOptions; ++a) {
        updateCellType(driver, a);
        const double costSizingEffectOnDelay =
            computeSizingEffectOnDriverCellDelay(
                i);  // sum(all arcs)(lambda * delay)

        const double cost =
            (alpha *
             orgCells.oCells[driver->footprintIndex].cells[a].leakagePower) +
            costSizingEffectOnDelay;
        lagrangianMap[costVectorIndex][a] = cost;
        // debug("tiago10", "\tCell: " << driver->instName << "(" <<
        // driver->instType << ")\tcost: " << lagrangianMap[costVectorIndex][a]
        // << "\tfor: " << a << endl;);
      }
      updateCellType(driver, lastOption);
      updateTimingDriverCell(i);
    }

    const int n = driver->sinkNetIndex;

    // cout << cell->instName << " resized to :" << bestOption << endl;
    // propagate back and evaluate tree extraction possibilities
    const int k0 = timingArcPointers[n];
    const int k1 = timingArcPointers[n + 1];
    for (int k = k0; k < k1; k++) {
      TimingArc &arc = timingArcs[k];
      TimingArcState &arcstate = getTimingArcState(k);
      Vcell *arcDriver = timingNets[arc.driver].driver;
      assert(driver == arc.cell);
      // const bool ignoredArc = ignoreArc(arc);
      const bool ignoredArc = ignoreArcOzdal(k, arcstate);

      // propagate back decision
      if (!ignoredArc) {
        q.push(arcDriver);
        s.push(arcDriver);
      }

    }  // end for
  }    // end while

  // cout << s.size() << endl;

  while (!s.empty()) {
    Vcell *driver = s.top();
    s.pop();

    assert(driver->treeIndex == treeIndex);

    if (driver->dontTouch) {
      continue;
    }

    // calculate edges weight, choose best option and propagate accumulated cost

    vector<EdgeArray<double>> originalDelays;

    const int i = driver->sinkNetIndex;
    const int costVectorIndex = lagrangianMapPointers[i];
    const int driverLastOption = driver->actualInstTypeIndex;

    const int libOptions = orgCells.oCells[driver->footprintIndex].cells.size();
    for (int a = 0; a < libOptions; ++a) {
      // compute cost for all previous cells (timingArcs)
      const int k0 = timingArcPointers[i];
      const int k1 = timingArcPointers[i + 1];
      for (int k = k0; k < k1; k++) {
        TimingArc &arc = timingArcs[k];
        TimingArcState &arcstate = getTimingArcState(k);
        Vcell *arcDriver = timingNets[arc.driver].driver;
        const int arcDriverCostVectorIndex = lagrangianMapPointers[arc.driver];
        double totalArcLambda = 0.0, maxArcLambda = 0.0,
               worstArcSlack = DBL_MAX;
        const int arcOutputPin = arcDriver->returnPinIndex("o");
        const int driverOutputPin = driver->returnPinIndex("o");

        // const bool ignoredArc = ignoreArc(arc);
        const bool ignoredArc = ignoreArcOzdal(k, arcstate);

        const int arcDriverLastOption = arcDriver->actualInstTypeIndex;

        int pin = arc.pin;
        const double deltaLoad = orgCells.oCells[driver->footprintIndex]
                                     .cells[a]
                                     .pins[pin]
                                     .capacitance -
                                 orgCells.oCells[driver->footprintIndex]
                                     .cells[driverLastOption]
                                     .pins[pin]
                                     .capacitance;

        int arcDriverBestOption = -1;
        double arcDriverBestCost = DBL_MAX;

        if ((arcDriver->dontTouch)) {  // arc driven by sequential cell or input
                                       // driver

          const double arcDriverMaxLoad =
              orgCells.oCells[arcDriver->footprintIndex]
                  .cells[arcDriverLastOption]
                  .pins[arcOutputPin]
                  .maxCapacitance;
          const double driverMaxLoad = orgCells.oCells[driver->footprintIndex]
                                           .cells[a]
                                           .pins[driverOutputPin]
                                           .maxCapacitance;
          assert(driverMaxLoad > 0.0);
          assert(arcDriverMaxLoad > 0.0);
          const double addedLoadViol =
              max(0.0,
                  ((arcDriver->actualLoad + deltaLoad) - arcDriverMaxLoad)) +
              max(0.0, driver->actualLoad - driverMaxLoad);

          const double sizingEffectOnSlew = deltaLoad * slewImpact(arc);

          const double arcDriverSizingEffectOnDelay =
              deltaLoad * delayImpact(arcDriver->sinkNetIndex);

          const double arcDriverCost =
              arcDriverSizingEffectOnDelay + sizingEffectOnSlew +
              max(loadViolPenalty * (addedLoadViol), 0.0) +
              lagrangianMap[arcDriverCostVectorIndex][arcDriverLastOption];

          arcDriverBestOption = arcDriverLastOption;
          arcDriverBestCost = arcDriverCost;

        } else {
          const int arcDriverLibOptions =
              orgCells.oCells[arcDriver->footprintIndex].cells.size();

          if (ignoredArc) {  // arc driven by cell not belonging to this tree

            const double arcDriverMaxLoad =
                orgCells.oCells[arcDriver->footprintIndex]
                    .cells[arcDriverLastOption]
                    .pins[arcOutputPin]
                    .maxCapacitance;
            const double driverMaxLoad = orgCells.oCells[driver->footprintIndex]
                                             .cells[a]
                                             .pins[driverOutputPin]
                                             .maxCapacitance;
            assert(driverMaxLoad > 0.0);
            assert(arcDriverMaxLoad > 0.0);
            const double addedLoadViol =
                max(0.0,
                    ((arcDriver->actualLoad + deltaLoad) - arcDriverMaxLoad)) +
                max(0.0, driver->actualLoad - driverMaxLoad);

            const double sizingEffectOnSlew = deltaLoad * slewImpact(arc);

            const double arcDriverSizingEffectOnDelay =
                deltaLoad * delayImpact(arcDriver->sinkNetIndex);

            const double arcDriverCost =
                arcDriverSizingEffectOnDelay + sizingEffectOnSlew +
                max(loadViolPenalty * (addedLoadViol), 0.0) +
                lagrangianMap[arcDriverCostVectorIndex][arcDriverLastOption];

            arcDriverBestOption = arcDriverLastOption;
            arcDriverBestCost = arcDriverCost;

          } else {  // arc driven by cell belonging to this tree

            arcDriverBestOption = -1;
            arcDriverBestCost = DBL_MAX;
            const double driverMaxLoad = orgCells.oCells[driver->footprintIndex]
                                             .cells[a]
                                             .pins[driverOutputPin]
                                             .maxCapacitance;
            for (int p = 0; p < arcDriverLibOptions; ++p) {
              const double arcDriverMaxLoad =
                  orgCells.oCells[arcDriver->footprintIndex]
                      .cells[p]
                      .pins[arcOutputPin]
                      .maxCapacitance;
              const double addedLoadViol =
                  max(0.0, ((arcDriver->actualLoad + deltaLoad) -
                            arcDriverMaxLoad)) +
                  max(0.0, driver->actualLoad - driverMaxLoad);

              assert(driverMaxLoad > 0.0);
              assert(arcDriverMaxLoad > 0.0);
              const double sizingEffectOnSlew = deltaLoad * slewImpact(arc);

              const double arcDriverSizingEffectOnDelay =
                  deltaLoad * delayImpact(arcDriver->sinkNetIndex);

              const double arcDriverCost =
                  arcDriverSizingEffectOnDelay + sizingEffectOnSlew +
                  max(loadViolPenalty * (addedLoadViol), 0.0) +
                  lagrangianMap[arcDriverCostVectorIndex][p];

              if (arcDriverCost < arcDriverBestCost) {
                arcDriverBestOption = p;
                arcDriverBestCost = arcDriverCost;
              }  // end if
            }    // end for
          }      // end else
        }        // end else
        lagrangianMap[costVectorIndex][a] +=
            /*lagrangianMap[arcDriverCostVectorIndex][arcDriverBestOption] +*/
            arcDriverBestCost;
        lagrangianMap[costVectorIndex + 1 + (k - k0)][a] = arcDriverBestOption;

      }  // end for (arcs)
    }    // end for (libOptions)

  }  // end while

  assert(q.empty());
  q.push(rootCell);
  // backtrack sizing cells
  while (!q.empty()) {
    Vcell *driver = q.front();
    q.pop();

    if (driver->dontTouch) continue;
    assert(timingNets[netIndex].driver->treeIndex == driver->treeIndex);
    int bestOption = -1;
    double bestCost = DBL_MAX;
    const int i = driver->sinkNetIndex;
    const int costVectorIndex = lagrangianMapPointers[i];

    // find best option
    const int libOptions = orgCells.oCells[driver->footprintIndex].cells.size();
    for (int a = 0; a < libOptions; ++a) {
      if (lagrangianMap[costVectorIndex][a] < bestCost) {
        bestOption = a;
        bestCost = lagrangianMap[costVectorIndex][a];
      }  // end if
    }    // end for

    updateCellType(driver, bestOption);
    ++changed;
    // cout << driver->instName << " resized to :" << bestOption << endl;
    // propagate back and evaluate tree extraction possibilities
    const int k0 = timingArcPointers[i];
    const int k1 = timingArcPointers[i + 1];
    for (int k = k0; k < k1; k++) {
      TimingArc &arc = timingArcs[k];
      Vcell *arcDriver = timingNets[arc.driver].driver;
      TimingArcState &arcstate = getTimingArcState(k);

      // const bool ignoredArc = ignoreArc(arc);
      const bool ignoredArc = ignoreArcOzdal(k, arcstate);

      // propagate back decision
      const int arcDriverCostVectorIndex = lagrangianMapPointers[arc.driver];
      if (/*(!arcDriver->dontTouch) && */(!ignoredArc) /*&& (lagrangianMap[costVectorIndex + 1 + k - k0][bestOption] != -1)*/) {
        lagrangianMap[arcDriverCostVectorIndex]
                     [lagrangianMap[costVectorIndex + 1 + k - k0][bestOption]] =
                         -DBL_MAX;
        q.push(arcDriver);
        s.push(arcDriver);
      }

    }  // end for
  }    // end while

  while (!s.empty()) {
    Vcell *driver = s.top();
    s.pop();

    assert(driver->treeIndex == treeIndex);

    const int i = driver->sinkNetIndex;
    updateTimingDriverCell(i);
    setDeltaLoadSlew(i);

    const double originalSizingEffect = computeSizingEffectOnDriverCellDelay(i);
    const double originalLeakage = driver->actualInstType->leakagePower;

    const int costVectorIndex = lagrangianMapPointers[i];
    const int lastOption = driver->actualInstTypeIndex;
    const int libOptions = orgCells.oCells[driver->footprintIndex].cells.size();

    if (driver->dontTouch) {
      lagrangianMap[costVectorIndex][lastOption] =
          originalLeakage + originalSizingEffect;
    } else {
      for (int a = 0; a < libOptions; ++a) {
        updateCellType(driver, a);
        const double costSizingEffectOnDelay =
            computeSizingEffectOnDriverCellDelay(
                i);  // sum(all arcs)(lambda * delay)

        const double cost =
            orgCells.oCells[driver->footprintIndex].cells[a].leakagePower +
            costSizingEffectOnDelay;
        lagrangianMap[costVectorIndex][a] = cost;
      }
      updateCellType(driver, lastOption);
      updateTimingDriverCell(i);
    }
  }  // end while

  // cout << changed << "\t";
  // char nada; cin >> nada;
}  // end method

// -----------------------------------------------------------------------------

void Circuit::backTrack2(const int netIndex) {
  // [WARNING] static is not thread-safe :)

  // evitar que se troque por mais lentas onde slack eh negativo e por mais
  // rapidas onde slack eh positivo
  static int treeIndex = 0;
  static int changed = 0;
  const double loadViolPenalty = pow(10.0, kIndex / 10.0);

  Vcell *rootCell = timingNets[netIndex].driver;
  const double alpha = 0.01 * kIndex;

  ++treeIndex;
  queue<Vcell *> q;
  stack<Vcell *> s;
  q.push(rootCell);
  s.push(rootCell);

  while (!q.empty()) {
    Vcell *driver = q.front();
    q.pop();

    assert(driver->treeIndex == -1);

    driver->treeIndex = treeIndex;

    if (driver->dontTouch) {
      continue;
    }

    const int i = driver->sinkNetIndex;

    updateTimingDriverCell(i);
    setDeltaLoadSlew(i);
    const double originalSizingEffect = computeSizingEffectOnDriverCellDelay(i);
    const double originalLeakage = driver->actualInstType->leakagePower;

    const int costVectorIndex = lagrangianMapPointers[i];
    const int lastOption = driver->actualInstTypeIndex;
    const int libOptions = orgCells.oCells[driver->footprintIndex].cells.size();

    if (driver->dontTouch) {
      lagrangianMap[costVectorIndex][lastOption] =
          alpha * originalLeakage + originalSizingEffect;
      // debug("tiago10", "\tCell: " << driver->instName << "(" <<
      // driver->instType << ")\tcost: " <<
      // lagrangianMap[costVectorIndex][lastOption] << endl;);
    } else {
      for (int a = 0; a < libOptions; ++a) {
        updateCellType(driver, a);
        const double costSizingEffectOnDelay =
            computeSizingEffectOnDriverCellDelay(
                i);  // sum(all arcs)(lambda * delay)

        const double cost =
            (alpha *
             orgCells.oCells[driver->footprintIndex].cells[a].leakagePower) +
            costSizingEffectOnDelay;
        lagrangianMap[costVectorIndex][a] = cost;
        // debug("tiago10", "\tCell: " << driver->instName << "(" <<
        // driver->instType << ")\tcost: " << lagrangianMap[costVectorIndex][a]
        // << "\tfor: " << a << endl;);
      }
      updateCellType(driver, lastOption);
      updateTimingDriverCell(i);
    }

    const int n = driver->sinkNetIndex;

    // cout << cell->instName << " resized to :" << bestOption << endl;
    // propagate back and evaluate tree extraction possibilities
    const int k0 = timingArcPointers[n];
    const int k1 = timingArcPointers[n + 1];
    for (int k = k0; k < k1; k++) {
      TimingArc &arc = timingArcs[k];
      TimingArcState &arcstate = getTimingArcState(k);
      Vcell *arcDriver = timingNets[arc.driver].driver;

      assert(driver == arc.cell);
      const bool ignoredArc = ignoreArcOzdal(k, arcstate);

      // propagate back decision
      if (!ignoredArc) {
        q.push(arcDriver);
        s.push(arcDriver);
      }

    }  // end for
  }    // end while

  while (!s.empty()) {
    Vcell *driver = s.top();
    s.pop();

    assert(driver->treeIndex == treeIndex);

    if (driver->dontTouch) {
      continue;
    }

    // calculate edges weight, choose best option and propagate accumulated cost

    vector<EdgeArray<double>> originalDelays;

    const int i = driver->sinkNetIndex;
    const int costVectorIndex = lagrangianMapPointers[i];
    const int driverLastOption = driver->actualInstTypeIndex;
    const double originalLambdaDelay =
        lagrangianMap[costVectorIndex][driverLastOption] -
        (alpha * orgCells.oCells[driver->footprintIndex]
                     .cells[driverLastOption]
                     .leakagePower);

    const int libOptions = orgCells.oCells[driver->footprintIndex].cells.size();
    for (int a = 0; a < libOptions; ++a) {
      const double wrongSizingPenalty = DBL_MAX / 1000;

      const double newLambdaDelay =
          lagrangianMap[costVectorIndex][a] -
          (alpha *
           orgCells.oCells[driver->footprintIndex].cells[a].leakagePower);

      if (hasNegativeSlack(i)) {
        if (newLambdaDelay > originalLambdaDelay)
          lagrangianMap[costVectorIndex][a] += wrongSizingPenalty;
      }
      // else if (newLambdaDelay < originalLambdaDelay)
      //        lagrangianMap[costVectorIndex][a] += wrongSizingPenalty;

      // compute cost for all previous cells (timingArcs)
      const int k0 = timingArcPointers[i];
      const int k1 = timingArcPointers[i + 1];
      for (int k = k0; k < k1; k++) {
        TimingArc &arc = timingArcs[k];
        TimingArcState &arcstate = getTimingArcState(k);

        Vcell *arcDriver = timingNets[arc.driver].driver;
        const int arcDriverCostVectorIndex = lagrangianMapPointers[arc.driver];
        double totalArcLambda = 0.0, maxArcLambda = 0.0,
               worstArcSlack = DBL_MAX;
        const int arcOutputPin = arcDriver->returnPinIndex("o");
        const int driverOutputPin = driver->returnPinIndex("o");

        const bool ignoredArc = ignoreArcOzdal(k, arcstate);

        const int arcDriverLastOption = arcDriver->actualInstTypeIndex;

        int pin = arc.pin;
        const double deltaLoad = orgCells.oCells[driver->footprintIndex]
                                     .cells[a]
                                     .pins[pin]
                                     .capacitance -
                                 orgCells.oCells[driver->footprintIndex]
                                     .cells[driverLastOption]
                                     .pins[pin]
                                     .capacitance;

        int arcDriverBestOption = -1;
        double arcDriverBestCost = DBL_MAX;

        if ((arcDriver->dontTouch)) {  // arc driven by sequential cell or input
                                       // driver

          const double arcDriverMaxLoad =
              orgCells.oCells[arcDriver->footprintIndex]
                  .cells[arcDriverLastOption]
                  .pins[arcOutputPin]
                  .maxCapacitance;
          const double driverMaxLoad = orgCells.oCells[driver->footprintIndex]
                                           .cells[a]
                                           .pins[driverOutputPin]
                                           .maxCapacitance;
          assert(driverMaxLoad > 0.0);
          assert(arcDriverMaxLoad > 0.0);
          const double addedLoadViol =
              max(0.0,
                  ((arcDriver->actualLoad + deltaLoad) - arcDriverMaxLoad)) +
              max(0.0, driver->actualLoad - driverMaxLoad);

          const double sizingEffectOnSlew = deltaLoad * slewImpact(arc);

          const double arcDriverSizingEffectOnDelay =
              deltaLoad * delayImpact(arcDriver->sinkNetIndex);

          const double arcDriverCost =
              arcDriverSizingEffectOnDelay + sizingEffectOnSlew +
              max(loadViolPenalty * (addedLoadViol), 0.0) +
              lagrangianMap[arcDriverCostVectorIndex][arcDriverLastOption];

          arcDriverBestOption = arcDriverLastOption;
          arcDriverBestCost = arcDriverCost;

        } else {
          const int arcDriverLibOptions =
              orgCells.oCells[arcDriver->footprintIndex].cells.size();

          if (ignoredArc) {  // arc driven by cell not belonging to this tree

            const double arcDriverMaxLoad =
                orgCells.oCells[arcDriver->footprintIndex]
                    .cells[arcDriverLastOption]
                    .pins[arcOutputPin]
                    .maxCapacitance;
            const double driverMaxLoad = orgCells.oCells[driver->footprintIndex]
                                             .cells[a]
                                             .pins[driverOutputPin]
                                             .maxCapacitance;
            assert(driverMaxLoad > 0.0);
            assert(arcDriverMaxLoad > 0.0);
            const double addedLoadViol =
                max(0.0,
                    ((arcDriver->actualLoad + deltaLoad) - arcDriverMaxLoad)) +
                max(0.0, driver->actualLoad - driverMaxLoad);

            const double sizingEffectOnSlew = deltaLoad * slewImpact(arc);

            const double arcDriverSizingEffectOnDelay =
                deltaLoad * delayImpact(arcDriver->sinkNetIndex);

            const double arcDriverCost =
                arcDriverSizingEffectOnDelay + sizingEffectOnSlew +
                max(loadViolPenalty * (addedLoadViol), 0.0) +
                lagrangianMap[arcDriverCostVectorIndex][arcDriverLastOption];

            arcDriverBestOption = arcDriverLastOption;
            arcDriverBestCost = arcDriverCost;

          } else {  // arc driven by cell belonging to this tree

            arcDriverBestOption = -1;
            arcDriverBestCost = DBL_MAX;
            const double driverMaxLoad = orgCells.oCells[driver->footprintIndex]
                                             .cells[a]
                                             .pins[driverOutputPin]
                                             .maxCapacitance;
            for (int p = 0; p < arcDriverLibOptions; ++p) {
              const double arcDriverMaxLoad =
                  orgCells.oCells[arcDriver->footprintIndex]
                      .cells[p]
                      .pins[arcOutputPin]
                      .maxCapacitance;
              const double addedLoadViol =
                  max(0.0, ((arcDriver->actualLoad + deltaLoad) -
                            arcDriverMaxLoad)) +
                  max(0.0, driver->actualLoad - driverMaxLoad);

              const double sizingEffectOnSlew = deltaLoad * slewImpact(arc);

              const double arcDriverSizingEffectOnDelay =
                  deltaLoad * delayImpact(arcDriver->sinkNetIndex);

              const double arcDriverCost =
                  arcDriverSizingEffectOnDelay + sizingEffectOnSlew +
                  max(loadViolPenalty * (addedLoadViol), 0.0) +
                  lagrangianMap[arcDriverCostVectorIndex][p];

              if (arcDriverCost < arcDriverBestCost) {
                arcDriverBestOption = p;
                arcDriverBestCost = arcDriverCost;
              }  // end if
            }    // end for
          }      // end else
        }        // end else
        lagrangianMap[costVectorIndex][a] +=
            /*lagrangianMap[arcDriverCostVectorIndex][arcDriverBestOption] +*/
            arcDriverBestCost;
        lagrangianMap[costVectorIndex + 1 + (k - k0)][a] = arcDriverBestOption;

      }  // end for (arcs)
    }    // end for (libOptions)

  }  // end while

  assert(q.empty());
  q.push(rootCell);
  // backtrack sizing cells
  while (!q.empty()) {
    Vcell *driver = q.front();
    q.pop();

    if (driver->dontTouch) continue;
    assert(timingNets[netIndex].driver->treeIndex == driver->treeIndex);
    int bestOption = -1;
    double bestCost = DBL_MAX;
    const int i = driver->sinkNetIndex;
    const int costVectorIndex = lagrangianMapPointers[i];

    // find best option
    const int libOptions = orgCells.oCells[driver->footprintIndex].cells.size();
    for (int a = 0; a < libOptions; ++a) {
      if (lagrangianMap[costVectorIndex][a] < bestCost) {
        bestOption = a;
        bestCost = lagrangianMap[costVectorIndex][a];
      }  // end if
    }    // end for

    updateCellType(driver, bestOption);
    ++changed;
    // cout << driver->instName << " resized to :" << bestOption << endl;
    // propagate back and evaluate tree extraction possibilities
    const int k0 = timingArcPointers[i];
    const int k1 = timingArcPointers[i + 1];
    for (int k = k0; k < k1; k++) {
      TimingArc &arc = timingArcs[k];
      TimingArcState &arcstate = getTimingArcState(k);

      Vcell *arcDriver = timingNets[arc.driver].driver;

      const bool ignoredArc = ignoreArcOzdal(k, arcstate);

      // propagate back decision
      const int arcDriverCostVectorIndex = lagrangianMapPointers[arc.driver];
      if (/*(!arcDriver->dontTouch) && */(!ignoredArc) /*&& (lagrangianMap[costVectorIndex + 1 + k - k0][bestOption] != -1)*/) {
        lagrangianMap[arcDriverCostVectorIndex]
                     [lagrangianMap[costVectorIndex + 1 + k - k0][bestOption]] =
                         -DBL_MAX;
        q.push(arcDriver);
        s.push(arcDriver);
      }

    }  // end for
  }    // end while

  while (!s.empty()) {
    Vcell *driver = s.top();
    s.pop();

    assert(driver->treeIndex == treeIndex);

    const int i = driver->sinkNetIndex;
    updateTimingDriverCell(i);
    setDeltaLoadSlew(i);

    const double originalSizingEffect = computeSizingEffectOnDriverCellDelay(i);
    const double originalLeakage = driver->actualInstType->leakagePower;

    const int costVectorIndex = lagrangianMapPointers[i];
    const int lastOption = driver->actualInstTypeIndex;
    const int libOptions = orgCells.oCells[driver->footprintIndex].cells.size();

    if (driver->dontTouch) {
      lagrangianMap[costVectorIndex][lastOption] =
          originalLeakage + originalSizingEffect;
    } else {
      for (int a = 0; a < libOptions; ++a) {
        updateCellType(driver, a);
        const double costSizingEffectOnDelay =
            computeSizingEffectOnDriverCellDelay(
                i);  // sum(all arcs)(lambda * delay)

        const double cost =
            orgCells.oCells[driver->footprintIndex].cells[a].leakagePower +
            costSizingEffectOnDelay;
        lagrangianMap[costVectorIndex][a] = cost;
      }
      updateCellType(driver, lastOption);
      updateTimingDriverCell(i);
    }
  }  // end while

  // cout << changed << "\t";
  // char nada; cin >> nada;
}  // end method

// -----------------------------------------------------------------------------

bool Circuit::ignoreArc(TimingArc &arc, TimingArcState &arcstate) {
  const double beta = 0.51;
  bool ignoredArc = false;
  double totalArcLambda = 0.0, maxArcLambda = 0.0, worstArcSlack = DBL_MAX;

  Vcell *arcDriver = timingNets[arc.driver].driver;

  // test sink arcs lambda to decide tree extraction
  const int arc0 = timingSinkArcPointers[arcDriver->sinkNetIndex];
  const int arc1 = timingSinkArcPointers[arcDriver->sinkNetIndex + 1];
  for (int arcIndex = arc0; arcIndex < arc1; ++arcIndex) {
    const TimingArc &arc2 = timingArcs[timingSinkArcs[arcIndex]];
    if (!arc2.cell) continue;
    TimingArcState &arcstate2 = getTimingArcState(timingSinkArcs[arcIndex]);

    totalArcLambda += arcstate2.lambda.getMax();
    maxArcLambda = max(maxArcLambda, arcstate2.lambda.getMax());
    worstArcSlack = min(worstArcSlack, arcstate2.slack.getMin());
  }
  // tree extraction criteria
  if (arcstate.lambda.getMax() < beta * totalArcLambda) {
    // if (arc.lambda.getMax() < maxArcLambda) {
    // if (arc.slack.getMin() > worstArcSlack) {
    // if (arcDriver->nextNum > 1) {
    ignoredArc = true;
  }

  // if (timingSinkNetPointers[i] == timingSinkNetPointers[i + 1]) {
  if (arc.cell->dontTouch) {
    ignoredArc = true;
  }

  return ignoredArc;
}  // end method

// -----------------------------------------------------------------------------

bool Circuit::ignoreArcOzdal(const int arcIndex, TimingArcState &arcstate) {
  const double beta = 3.0;
  bool ignoredArc = false;
  double totalArcLambda = 0.0, maxArcLambda = 0.0, worstArcSlack = DBL_MAX;

  TimingArc &arc = timingArcs[arcIndex];
  Vcell *arcDriver = timingNets[arc.driver].driver;

  TimingNet &net = timingNets[arcDriver->sinkNetIndex];

  const int a0 = timingSinkArcPointers[arcDriver->sinkNetIndex];
  const int a1 = timingSinkArcPointers[arcDriver->sinkNetIndex + 1];
  int maxArcIndex = timingSinkArcs[a0];
  for (int a = a0; a < a1; ++a) {
    TimingArc &arc = timingArcs[timingSinkArcs[a]];
    TimingArcState &arcstate2 = getTimingArcState(timingSinkArcs[a]);
    TimingArcState &maxarcstate2 =
        getTimingArcState(timingSinkArcs[maxArcIndex]);
    maxArcLambda = max(maxArcLambda, arcstate2.lambda.getMax());
    if (arcstate2.lambda.getMax() > maxarcstate2.lambda.getMax()) {
      maxArcIndex = timingSinkArcs[a];
    }
  }

  bool root = false;
  for (int a = a0; a < a1; ++a) {
    TimingArc &arc = timingArcs[a];
    TimingArcState &arcstate2 = getTimingArcState(timingSinkArcs[a]);
    TimingArcState &maxarcstate2 =
        getTimingArcState(timingSinkArcs[maxArcIndex]);
    if (timingSinkArcs[a] != maxArcIndex) {
      if (arcstate2.lambda.getMax() * beta > maxarcstate2.lambda.getMax()) {
        root = true;
        break;
      }
    }
  }

  if ((!root) && (maxArcIndex == arcIndex)) return false;

  return true;
}  // end method

// -----------------------------------------------------------------------------

void Circuit::LR() {
  // Ozdal's 2011/2012 LR sizing implementation

  LRDPInitialization();

  referenceLeakage = totalLeakage;

  sizingForNoLoadViolation();
  updateTiming();

  setInitialLambdaKKT();
  printTimingDigest(true, "LR-DP");

  updateLambdasByTennakoon();

  int iteration = 0;
  while (iteration < 200) {
    ++iteration;
    criticalTreeExtraction();
    printTimingDigest();
    // updateLambdasByOzdal();
    // updateLambdasByTennakoon();
    updateLambdasByFlach();
    ++kIndex;
  }
  cout << "\n\nterminou LR" << endl;
  printTiming();

}  // end method

// -----------------------------------------------------------------------------

void Circuit::LR_PT() {
  updateRequiredTimeLR_PT_KKT();
  setInitialLambdaKKT();

  callPT();
  readTimingLR();

  int iteration = 0;
  double lastLeakage = DBL_MAX;
  while (totalLeakage != lastLeakage) {
    lastLeakage = totalLeakage;
    ++iteration;
    runDP();
    callPT();
    readTimingLR();
    updateRequiredTimeLR_PT_KKT();
  }

  cout << "\n\nterminou LR" << endl;
  char nada;
  cin >> nada;

}  // end method

// -----------------------------------------------------------------------------

void Circuit::setDeltaLoadSlew(const int netIndex) {
  // calculate delay and output slew variations by output load and input slew
  // variations

  const TimingNet &net = timingNets[netIndex];
  Vcell *driver = net.driver;
  if (!driver) return;

  const TimingNetState &netstate = getTimingNetState(netIndex);

  const int k0 = timingArcPointers[netIndex];
  const int k1 = timingArcPointers[netIndex + 1];
  for (int k = k0; k < k1; k++) {
    // Update arc timings.
    const TimingArc &arc = timingArcs[k];
    TimingArcState &arcstate = getTimingArcState(k);

    const LibParserTimingInfo &timingInfo =
        arc.cell->actualInstType->timingArcs[arc.lut];

    assert(arc.sink == netIndex);

    EdgeArray<double> refDelay = arcstate.delay;
    EdgeArray<double> refSlew = arcstate.oslew;
    // args:          cellSize,      input slew, output load,    (result),
    // (result)
    computeArcTiming(timingInfo, arcstate.islew, netstate.load, refDelay,
                     refSlew);

    computeArcTiming(timingInfo, arcstate.islew, netstate.load + 1.0,
                     arcstate.delay, arcstate.oslew);

    deltaDelay_deltaLoad[k] = arcstate.delay - refDelay;
    deltaSlew_deltaLoad[k] = arcstate.oslew - refSlew;

    computeArcTiming(timingInfo, arcstate.islew + 1.0, netstate.load,
                     arcstate.delay, arcstate.oslew);

    deltaDelay_deltaSlew[k] = arcstate.delay - refDelay;

  }  // end for
}  // end method

// -----------------------------------------------------------------------------

double Circuit::evaluateLRS() {
  // T  -> target delay (clock period)
  // Di -> timing arc delay
  // aj -> arrival time (reversed) at arc input
  // ai -> arrival time at arc output, i.e, arrival time at arc's sink net

  // Notice: aj + Di != ai
  updateRequiredTime();
  const double T = sdcInfos.clk_period;
  double LRS = 0.0;
  // Output Timing Arcs (tail)
  const int numArcs = timingArcs.size();
  const EdgeArray<double> clk(T, T);
  const EdgeArray<double> lambdaMin(1e-5, 1e-5);
  double div = 0.0;
  // Combinational Timing Arcs (body)
  for (int i = timingOffsetToCombinationArcs;
       i < timingOffsetToExtraSequentialArcs; i++) {
    const TimingArc &arc = timingArcs[i];
    const TimingArcState &arcstate = getTimingArcState(i);

    const EdgeArray<double> mv =
        getTimingNetState(arc.sink).arrivalTime.getReversed() -
        timingRequiredTime[arc.sink].getReversed();
    const EdgeArray<double> mu_v = -arcstate.slack;

    int n = arc.sink;
    if (timingSinkNetPointers[n] == timingSinkNetPointers[n + 1]) {
      LRS += (arcstate.lambda * (-arcstate.slack)).aggregate() +
             arc.cell->actualInstType->leakagePower;
      div += ((-arcstate.slack) * (-arcstate.slack)).aggregate();
    } else {
      LRS += (arcstate.lambda * (mu_v - mv)).aggregate() +
             arc.cell->actualInstType->leakagePower;
      div += ((mu_v - mv) * (mu_v - mv)).aggregate();
    }

  }  // end for
  const double upperBound = 10.0 * referenceLeakage;
  const double delta = 2.0 * (upperBound - LRS) / (div);
  return delta;

}  // end method

// -----------------------------------------------------------------------------

void Circuit::callPT() {
  // [TODO] Reimann: set pollingTime according to circuit size (avoid too many
  // calls to sleep command)
  const double pollingTime = 1.0;

  // Read each sizes file and run timing analysis
  // Read sizes
  vector<pair<string, string>> sizes = copySizes();
  // Run timing analysis
  cout << " Running timing analysis on " << benchName << "..." << endl;
  TimerInterface::Status s = TimerInterface::runTimingAnalysisBlocking(
      sizes, rootDir, benchName, pollingTime);
  cout << "  Timing analysis finished with status: " << s << endl;
  if (TimerInterface::TIMER_FINISHED_SUCCESS != s) {
    cout << " -E-: Something went wrong, exiting. Check pt.log in "
            "ISPD_CONTEST_ROOT directory"
         << endl;
    cout << " Trying again..." << endl;
    callPT();
  }
}

// -----------------------------------------------------------------------------

void Circuit::updateLambdasByOzdal() {
  updateRequiredTime();
  const double T = sdcInfos.clk_period;
  double pk = fabs(evaluateLRS());
  cout << pk << endl;
  double lambdaTotal = 0.0;
  // Output Timing Arcs (tail)
  const int numArcs = timingArcs.size();
  const EdgeArray<double> clk(T, T);
  const EdgeArray<double> lambdaMin(1e-15, 1e-15);

  for (int i = timingOffsetToExtraSequentialArcs; i < numArcs; i++) {
    const TimingArc &arc = timingArcs[i];
    TimingArcState &arcstate = getTimingArcState(i);

    arcstate.slack = clk - arcstate.arrivalTime;
    // cout << arc.lambda << "\t";
    arcstate.lambda += pk * (-arcstate.slack);
    arcstate.lambda = max(lambdaMin, arcstate.lambda);
    lambdaTotal += arcstate.lambda.aggregate();
    if (arc.cell)
      debug("tiago1", arc.cell->instName << "\tlambdas: " << arcstate.lambda
                                         << "\tarc slacks: " << -arcstate.slack
                                         << endl);
    else
      debug("tiago1", "NO CELL!"
                          << "\tlambdas: " << arcstate.lambda
                          << "\tarc slacks: " << -arcstate.slack << endl);
  }  // end for

  // Combinational Timing Arcs (body)
  for (int i = timingOffsetToCombinationArcs;
       i < timingOffsetToExtraSequentialArcs; i++) {
    const TimingArc &arc = timingArcs[i];
    TimingArcState &arcstate = getTimingArcState(i);

    const EdgeArray<double> mv =
        getTimingNetState(arc.sink).arrivalTime.getReversed() -
        timingRequiredTime[arc.sink].getReversed();
    const EdgeArray<double> mu_v = -arcstate.slack;

    int n = arc.sink;
    // if (timingSinkNetPointers[n] == timingSinkNetPointers[n + 1])
    //    arc.lambda += pk * (mv);
    // else
    arcstate.lambda += pk * (mu_v - mv);

    arcstate.lambda = max(lambdaMin, arcstate.lambda);
    lambdaTotal += arcstate.lambda.aggregate();
    debug("tiago1", arc.cell->instName << "\tlambdas: " << arcstate.lambda
                                       << "\t mu_v: " << mu_v << "\t mv: " << mv
                                       << "\tslacks: " << arcstate.slack
                                       << endl);
  }  // end for

  // Input Timing Arcs (head)
  for (int i = 0; i < timingOffsetToCombinationArcs; i++) {
    const TimingArc &arc = timingArcs[i];
    TimingArcState &arcstate = getTimingArcState(i);

    const EdgeArray<double> mv =
        getTimingNetState(arc.sink).arrivalTime.getReversed() -
        timingRequiredTime[arc.sink].getReversed();
    const EdgeArray<double> mu_v = -arcstate.slack;

    arcstate.lambda += pk * (mu_v - mv);
    arcstate.lambda = max(lambdaMin, arcstate.lambda);
    lambdaTotal += arcstate.lambda.aggregate();

  }  // end for
  updateLambdas_KKT();
  return;
  for (int i = timingOffsetToExtraSequentialArcs; i < numArcs; i++) {
    const TimingArc &arc = timingArcs[i];
    const TimingArcState &arcstate = getTimingArcState(i);

    if (arc.cell)
      debug("tiago1", arc.cell->instName << "\tlambdas: " << arcstate.lambda
                                         << "\tarc slacks: " << arcstate.slack
                                         << endl);
    else
      debug("tiago1", "NO CELL!"
                          << "\tlambdas: " << arcstate.lambda
                          << "\tarc slacks: " << arcstate.slack << endl);
  }  // end for

  // Combinational Timing Arcs (body)
  for (int i = timingOffsetToCombinationArcs;
       i < timingOffsetToExtraSequentialArcs; i++) {
    const TimingArc &arc = timingArcs[i];
    const TimingArcState &arcstate = getTimingArcState(i);

    if (arc.cell)
      debug("tiago1", arc.cell->instName << "\tlambdas: " << arcstate.lambda
                                         << "\tarc slacks: " << arcstate.slack
                                         << endl);
  }  // end for
     // cout << lambdaTotal << endl;
  // cin >> nada;

}  // end method

// -----------------------------------------------------------------------------

void Circuit::LRDPInitialization() {
  // Pointers to the respective cost/size vector
  // Same number of timingArcs of the net plus one vector to store accumulated
  // costs dummy nets kept to keep code consistency
  const int mapPointersSize = timingNets.size() + 1;
  lagrangianMapPointers.resize(mapPointersSize);
  int lagrangianMapPointerCounter = 0;
  for (int i = 0; i < mapPointersSize; ++i) {
    const int k0 = timingArcPointers[i];
    const int k1 = timingArcPointers[i + 1];
    lagrangianMapPointers[i] = lagrangianMapPointerCounter;
    lagrangianMapPointerCounter +=
        1 + (k1 - k0);  // num of timingArcs + 1 for cost vector
  }

  // cout << timingOffsetToExtraSequentialArcs << " " <<
  // timingOffsetToExtraPrimaryOutputArcs << endl;
  for (int i = timingOffsetToCombinationArcs;
       i < timingOffsetToExtraSequentialArcs; i++) {
    TimingArc &arc = timingArcs[i];
    // cout << i << " " << arc.driver << " " << arc.cell->instName << endl;
    Vcell *arcDriver = timingNets[arc.driver].driver;
    // cout << "Foi " << arcDriver->instName << endl;
  }
  // The cost/size vector. Stores the information needed by DP to traverse
  // the circuit minimizing the LR function. For each net (cell) stores the
  // accumulated cost and the corresponding size of previous cells. The second
  // index represents the cell size index
  const int mapSize = timingNets.size() + timingArcs.size();
  lagrangianMap.resize(mapSize);

  // resizing map vector according to lib cell options
  // dummy nets dont have driver, so dont need map resizing
  for (int i = timingNumDummyNets; i < mapPointersSize - 1; ++i) {
    Vcell *driver = timingNets[i].driver;
    const int libOptions = orgCells.oCells[driver->footprintIndex].cells.size();
    // cout << libOptions << endl;
    const int k0 = lagrangianMapPointers[i];
    const int k1 = lagrangianMapPointers[i + 1];
    // cout << "resizing net.driver: " << driver->instName << " " << k0 << " "
    // << k1 << endl;
    for (int k = k0; k < k1; k++) {
      lagrangianMap[k].resize(libOptions, 0.0);
      // cout << "resizing net.driver: " << driver->instName << " cost/arc: " <<
      // k << endl;
    }
  }

  deltaDelay_deltaLoad.resize(timingArcs.size(), EdgeArray<double>(0.0, 0.0));
  deltaSlew_deltaLoad.resize(timingArcs.size(), EdgeArray<double>(0.0, 0.0));
  deltaDelay_deltaSlew.resize(timingArcs.size(), EdgeArray<double>(0.0, 0.0));

}  // end method

// -----------------------------------------------------------------------------

void Circuit::setInitialLambda() {
  const double lambdaMin =
      1.0e6;  // timingWorstArrivalTime.getMax()-sdcInfos.clk_period;
  cout << "Lambda min.: " << lambdaMin << endl;

  const double lambdaInit = lambdaMin;
  cout << "Lambda init.: " << lambdaInit << endl;
  const int arcSize = timingArcs.size();
  for (int i = 0; i < arcSize; ++i) {
    // Update arc timings.
    TimingArcState &arcstate = getTimingArcState(i);
    // cout << arc.cell->instName << "\t" << arc.cell->instType << "\t" <<
    // arc.cell->logicalDepth <<  "\t" << arc.driver << endl;
    arcstate.lambda.set(lambdaInit, lambdaInit);
  }

}  // end method

// -----------------------------------------------------------------------------

void Circuit::setInitialLambdaKKT() {
  const double lambdaMin =
      1.0;  // 0.01*(timingWorstArrivalTime.getMax()-sdcInfos.clk_period)/sdcInfos.clk_period;
  cout << "Lambda min.: " << lambdaMin << endl;
  cout << "Reference Leakage: " << referenceLeakage << endl;
  const int arcSize = timingArcs.size();
  for (int i = 0; i < arcSize; ++i) {
    // Update arc timings.
    TimingArcState &arcstate = getTimingArcState(i);
    // cout << arc.cell->instName << "\t" << arc.cell->instType << "\t" <<
    // arc.cell->logicalDepth <<  "\t" << arc.driver << endl;
    arcstate.lambda.set(lambdaMin, lambdaMin);
  }
  updateLambdas_KKT();
}  // end method

// =============================================================================
// PhD Thesis Flow
// =============================================================================

void Circuit::sizingDescreaseVthOfBottleneckCells() {
  const int numNets = timingNets.size();

  std::vector<pair<int, int>> sorting(numNets);  // num paths, net

  for (int i = 0; i < numNets; i++) {
    const TimingNet &net = timingNets[i];
    sorting[i] = make_pair(net.ipaths * net.opaths, i);
  }  // end for

  std::sort(sorting.begin(), sorting.end(), std::greater<pair<int, int>>());

  const double alpha = 0.1;
  const int lvthThreshold = numNets * alpha;

  for (int i = 0; i < lvthThreshold; i++) {
    const int n = sorting[i].second;

    const TimingNet &net = timingNets[n];
    Vcell *driver = net.driver;
    if (!driver || driver->dontTouch) continue;

    const TimingNetState &netstate = getTimingNetState(i);

    const double riseSlack =
        -(netstate.arrivalTime[RISE] - timingRequiredTime[n][RISE]);
    const double fallSlack =
        -(netstate.arrivalTime[FALL] - timingRequiredTime[n][FALL]);
    const double slack = min(riseSlack, fallSlack);

    cout << timingNetName[n] << " " << sorting[i].first << " " << slack << "\n";

    decreaseVth(driver, false);
    decreaseVth(driver, false);
  }  // end for
}  // end void

//------------------------------------------------------------------------------
// Logical Effort
//------------------------------------------------------------------------------

double Circuit::computeLogicalEffortDelay(const int k, const double G,
                                          const double B, const double P,
                                          const double H) const {
  return k * pow(G * B * H, 1.0 / k) + P;
}  // end method

//------------------------------------------------------------------------------

void Circuit::logicalEffort_Reset_Cin(const int k) {
  const TimingArc &arc = timingArcs[k];
  TimingArcState &arcstate = getTimingArcState(k);

  if (arc.cell) {
    arcstate.logicalEffortCin = getLibPinCapacitanceFromTimingArc(arc);

    if (k < timingOffsetToExtraSequentialArcs) {
      if (arc.cell->actualInstType->isSequential ||
          arc.cell->instName == "inputDriver") {
        for (int edge = 0; edge < 2; edge++) {
          LogicalEffortPath &path = arcstate.logicalEffortPath[edge];
          path.pathLogicalEffort = 1;
          path.pathBranching = 1;
          path.pathCin = arcstate.logicalEffortCin;
          path.pathParasiticDelay = 0;
          path.pathDepth = 0;
        }  // end for
      }    // end if
    }      // end if
  }        // end if
}  // end method

// -----------------------------------------------------------------------------

void Circuit::logicalEffort_UpdateNetLoad(const int n) {
  const TimingNet &net = timingNets[n];
  TimingNetState &netstate = getTimingNetState(n);

  netstate.logicalEffortLoad =
      net.driver ? net.driver->wireLoad + net.driver->portLoad : 0;

  const int j0 = timingSinkArcPointers[n];
  const int j1 = timingSinkArcPointers[n + 1];
  for (int j = j0; j < j1; j++) {
    const int k = timingSinkArcs[j];
    const TimingArc &arc = timingArcs[k];
    const TimingArcState &arcstate = getTimingArcState(k);
    if (arc.cell) netstate.logicalEffortLoad += arcstate.logicalEffortCin;
  }  // end for
}  // end method

//------------------------------------------------------------------------------

void Circuit::logicalEffort_Reset() {
  const int numArcs = timingArcs.size();
  for (int k = 0; k < numArcs; k++) {
    logicalEffort_Reset_Cin(k);
  }  // end for

  const int numNets = timingNets.size();
  for (int n = 0; n < numNets; n++) {
    logicalEffort_UpdateNetLoad(n);
  }  // end for
}  // end method

//------------------------------------------------------------------------------

void Circuit::logicalEffort_Propagate() {
  if (enableReport_LogicalEffortPropagation) {
    reportFile_LogicalEffortPropagation << "Starting:\n";
  }  // end if

  // Initialize arcs.
  for (int k = 0; k < timingOffsetToCombinationArcs; k++) {
    const TimingArc &arc = timingArcs[k];
    TimingArcState &arcstate = getTimingArcState(k);

    std::cout << arc.cell->instName << " (" << arc.cell->instType << ") "
              << arcstate.logicalEffortCin << "\n";
  }  // end for

  // From inputs to outputs.
  const int numNets = timingNets.size();
  for (int n = timingNumDummyNets; n < numNets; n++) {
    const TimingNet &net = timingNets[n];
    TimingNetState &netstate = getTimingNetState(n);

    const int k0 = timingArcPointers[n];
    const int k1 = timingArcPointers[n + 1];

    if (k0 == k1) {
      continue;
    } else {
      EdgeArray<LogicalEffortPath> worstPath;
      EdgeArray<double> worstDelay(-numeric_limits<double>::max(),
                                   -numeric_limits<double>::max());
      EdgeArray<int> worstBacktrack(-1, -1);

      for (int k = k0; k < k1; k++) {
        const TimingArc &arc = timingArcs[k];
        const TimingArcState &arcstate = getTimingArcState(k);

        for (int iedge = 0; iedge < 2; iedge++) {
          const int oedge = ((EdgeType)(iedge)).getReversed();
          const LogicalEffortPath &pathFrom = arcstate.logicalEffortPath[iedge];

          const LogicalEffort &arcLogicalEffort =
              (arc.cell->dontTouch) ? getLibArcLogicalEffortForCellSpecific(arc)
                                    : getLibArcLogicalEffortForCellGroup(arc);

          const double H = pathFrom.pathCin > 0
                               ? netstate.logicalEffortLoad / pathFrom.pathCin
                               : defaultH;

          LogicalEffortPath pathTo;
          pathTo.pathLogicalEffort =
              pathFrom.pathLogicalEffort * arcLogicalEffort.g[oedge];
          pathTo.pathBranching =
              pathFrom.pathDepth > 0 ? pathFrom.pathBranching : 1;
          pathTo.pathParasiticDelay =
              pathFrom.pathParasiticDelay + arcLogicalEffort.p[oedge];
          pathTo.pathCin = pathFrom.pathCin;
          pathTo.pathDepth = pathFrom.pathDepth + 1;

          const double delay = computeLogicalEffortDelay(
              pathTo.pathDepth, pathTo.pathLogicalEffort, pathTo.pathBranching,
              pathTo.pathParasiticDelay, H);

          if (delay > worstDelay[oedge]) {
            worstDelay[oedge] = delay;
            worstPath[oedge] = pathTo;
            worstBacktrack[oedge] = k;
          }  // end if
        }    // end for
      }      // end for

      netstate.logicalEffortBacktrack = worstBacktrack;
      netstate.logicalEffortPath = worstPath;
    }  // end else

    const int j0 = timingSinkArcPointers[n];
    const int j1 = timingSinkArcPointers[n + 1];
    for (int j = j0; j < j1; j++) {
      const int k = timingSinkArcs[j];
      const TimingArc &arc = timingArcs[k];
      TimingArcState &arcstate = getTimingArcState(k);

      const double branching = isClockNet(n) ? 1
                                             : netstate.logicalEffortLoad /
                                                   arcstate.logicalEffortCin;

      arcstate.logicalEffortPath = netstate.logicalEffortPath;
      arcstate.logicalEffortPath[FALL].pathBranching *= branching;
      arcstate.logicalEffortPath[RISE].pathBranching *= branching;
    }  // end for

    if (enableReport_LogicalEffortPropagation) {
      reportFile_LogicalEffortPropagation
          << "net " << (timingNetName[n] == "" ? "<no name>" : timingNetName[n])
          << " ";
      reportFile_LogicalEffortPropagation
          << (net.driver ? net.driver->instType : "<null>") << " ";
      for (int edge = 0; edge < 2; edge++) {
        const string edgename = (edge == FALL) ? "f" : "r";

        reportFile_LogicalEffortPropagation
            << "k" << edgename << "="
            << netstate.logicalEffortPath[edge].pathDepth << " ";
        reportFile_LogicalEffortPropagation
            << "G" << edgename << "="
            << netstate.logicalEffortPath[edge].pathLogicalEffort << " ";
        reportFile_LogicalEffortPropagation
            << "B" << edgename << "="
            << netstate.logicalEffortPath[edge].pathBranching << " ";
        reportFile_LogicalEffortPropagation
            << "P" << edgename << "="
            << netstate.logicalEffortPath[edge].pathParasiticDelay << " ";
        reportFile_LogicalEffortPropagation
            << "Cin" << edgename << "="
            << netstate.logicalEffortPath[edge].pathCin << " ";
      }  // end for
      reportFile_LogicalEffortPropagation << "\n";
    }  // end if
  }    // end for

  if (enableReport_LogicalEffortPropagation) {
    reportFile_LogicalEffortPropagation.close();
  }  // end if

}  // end method

// -----------------------------------------------------------------------------

void Circuit::logicalEffort_Sizing() {
  const bool report = true;
  ofstream file;

  if (report) {
    file.open("sizing-logical-effort.report");
  }  // end if

  const int numNets = timingNets.size();
  for (int i = numNets - 1; i >= timingNumDummyNets; i--) {
    const TimingNet &net = timingNets[i];
    TimingNetState &netstate = getTimingNetState(i);

    // Update net load.
    logicalEffort_UpdateNetLoad(i);

    if (report) {
      file << "cell " << (net.driver ? net.driver->instName : "null") << " ("
           << (net.driver ? net.driver->instType : "null") << ")\n";
      file << "\tdepth " << net.depth << "\n";
      file << "\tnet " << timingNetName[i] << "\n";
      file << "\tsinks " << net.fanout << "\n";
      file << "\tload " << netstate.load << "\n";
      file << "\tload (logical effort) " << netstate.logicalEffortLoad << "\n";
      file << "\tpaths " << (net.ipaths * net.opaths) << " (" << net.ipaths
           << "*" << net.opaths << ")\n";
    }  // end if

    // Compute stage effort of the worst path up to this net.
    double worstDelay = -numeric_limits<double>::max();
    double worstStageEffort = 0;

    for (int edge = 0; edge < 2; edge++) {
      const LogicalEffortPath &path = netstate.logicalEffortPath[edge];
      const int k = path.pathDepth;
      const double G = path.pathLogicalEffort;
      const double B = path.pathBranching;
      const double H = netstate.logicalEffortLoad /
                       (path.pathCin > 0 ? path.pathCin : defaultCin);
      const double P = path.pathParasiticDelay;

      const double f = pow(G * B * H, 1.0 / k);
      const double delay = k * f + P;
      if (delay > worstDelay) {
        worstDelay = delay;
        worstStageEffort = f;
      }  // end if

      //			std::cout << "k=" << k << " "
      //					<< "G=" << G << " "
      //					<< "B=" << B << " "
      //					<< "H=" << H << " "
      //					<< "P=" << P << " "
      //					<< "f=" << f << " "
      //					<< "d=" << delay << "\n";

    }  // end for

    if (report) {
      file << "\tworst delay=" << worstDelay << "\n";
      file << "\tworst stage effort=" << worstStageEffort << "\n";
    }  // end if

    double smallerGain = +numeric_limits<double>::max();

    for (int edge = 0; edge < 2; edge++) {
      const int k = netstate.logicalEffortBacktrack[edge];
      const TimingArc &arc = timingArcs[k];
      const LibCellCharacterization &cellChar =
          getLibCellCharacterization(net.driver);
      const LibArcCharacterization &arcChar = cellChar.arcs[arc.lut];

      const TimingArcState &arcstate = getTimingArcState(k);
      const double h = worstStageEffort / arcChar.g[edge];
      if (h < smallerGain) {
        smallerGain = h;
      }  // end if
    }    // end for

    //		std::cout << "net " << timingNetName[i] << " "
    //				<< net.driver->instName << " (" <<
    // net.driver->instType
    //<<
    //")
    //"
    //				<< "worst_delay=" << worstDelay << " "
    //				<< "worst_f=" << worstStageEffort << " "
    //				<< "smaller_gain=" << smallerGain << " "
    //				<< "load=" << netstate.logicalEffortLoad
    //				<< "\n";

    if (report) {
      file << "\tworst gain=" << smallerGain << "\n";
    }  // end if

    // Resize all pins based on the worst gain.
    const int k0 = timingArcPointers[i];
    const int k1 = timingArcPointers[i + 1];
    for (int k = k0; k < k1; k++) {
      TimingArcState &arcstate = getTimingArcState(k);
      arcstate.logicalEffortCin = netstate.logicalEffortLoad / smallerGain;

      if (report) {
        const TimingArc &arc = timingArcs[k];
        file << "\tpin " << getLibPinName(arc) << ": "
             << "cin=" << arcstate.logicalEffortCin << "\n";
      }  // end if

    }  // end for

  }  // end for

}  // end method

// -----------------------------------------------------------------------------

void Circuit::logicalEffort_Report_Delays() {
  const double T = getT();

  ofstream file("le-delays.report");

  const int numArcs = timingArcs.size();
  for (int k = 0; k < numArcs; k++) {
    const TimingArc &arc = timingArcs[k];
    const TimingArcState &arcstate = getTimingArcState(k);

    double load = numeric_limits<double>::quiet_NaN();
    double h = numeric_limits<double>::quiet_NaN();
    if (arc.sink != -1) {
      const TimingNetState &netstate = getTimingNetState(arc.sink);
      load = netstate.load;
      h = logicalEffort_GetGain(netstate, arcstate);
    }

    EdgeArray<double> g(numeric_limits<double>::quiet_NaN(),
                        numeric_limits<double>::quiet_NaN());
    EdgeArray<double> p(numeric_limits<double>::quiet_NaN(),
                        numeric_limits<double>::quiet_NaN());
    if (arc.cell) {
      const LibCellCharacterization &cellChar =
          getLibCellCharacterization(arc.cell);
      if (arc.lut != -1) {
        const LibArcCharacterization &arcChar = cellChar.arcs[arc.lut];

        g = arcChar.g;
        p = arcChar.p;
      }  // end if
    }    // end if

    file << "pin " << (arc.cell ? arc.cell->instName : "<null>") << ":"
         << getLibPinName(arc) << " ";
    file << (arc.cell ? arc.cell->instType : "<null>") << " ";
    file << "slew=" << arcstate.islew << " ";
    file << "p=" << p << " ";
    file << "g=" << g << " ";
    file << "h=" << h << " ";
    file << "load=" << load << " ";
    file << "timer=" << arcstate.delay << " ";
    file << "logical_effort=" << arcstate.logicalEffortDelay << " ";
    file << "diff=" << (arcstate.logicalEffortDelay - arcstate.delay) << "\n";
  }  // end for
}  // end method

// -----------------------------------------------------------------------------

void Circuit::logicalEffort_Report_ArrivaTimes() {
  const double T = getT();

  ofstream file("le-arrival-times.report");

  const int numArcs = timingArcs.size();
  for (int k = 0; k < numArcs; k++) {
    const TimingArc &arc = timingArcs[k];
    const TimingArcState &arcstate = getTimingArcState(k);

    file << "pin " << (arc.cell ? arc.cell->instName : "<null>") << ":"
         << getLibPinName(arc) << " ";
    file << "timer=" << arcstate.arrivalTime << " ";
    file << "logical_effort=" << arcstate.logicalEffortArrivalTime << " ";
    file << "diff="
         << (arcstate.logicalEffortArrivalTime - arcstate.arrivalTime) << "\n";
  }  // end for
}  // end method

// -----------------------------------------------------------------------------

void Circuit::logicalEffort_UpdateTiming_Wns() {
  const double T = getT();

  logicalEffortTotalNegativeSlack = 0;
  logicalEffortTotalPositiveSlack = 0;
  logicalEffortTotalAbsoluteSlack = 0;

  logicalEffortWorstArrivalTime.set(-numeric_limits<double>::max(),
                                    -numeric_limits<double>::max());
  logicalEffortWorstArrivalTimeNet.set(-1, -1);

  logicalEffortNumPathsWithNegativeSlack = 0;

  const int numTails = timingTailNets.size();
  for (int i = 0; i < numTails; i++) {
    const int netindex = timingTailNets[i];
    const TimingNetState &netstate = getTimingNetState(netindex);
    for (int edge = 0; edge < 2; edge++) {
      if (netstate.logicalEffortArrivalTime[edge] >
          logicalEffortWorstArrivalTime[edge]) {
        logicalEffortWorstArrivalTime[edge] =
            netstate.logicalEffortArrivalTime[edge];
        logicalEffortWorstArrivalTimeNet[edge] = netindex;
      }  // end if

      const double slack = timingTailNetMultiplicities[i] *
                           (T - netstate.logicalEffortArrivalTime[edge]);

      if (slack < 0) {
        logicalEffortTotalNegativeSlack -= slack;
        logicalEffortNumPathsWithNegativeSlack++;
      } else {
        logicalEffortTotalPositiveSlack += slack;
      }  // end else

      logicalEffortTotalAbsoluteSlack += fabs(slack);
    }  // end for
  }    // end for
}  // end method

// -----------------------------------------------------------------------------

void Circuit::logicalEffort_UpdateTiming_Net_Endpoint(const int i) {
  const TimingNetState &netstate = getTimingNetState(i);

  const int k0 = timingSinkArcPointers[i];
  const int k1 = timingSinkArcPointers[i + 1];
  for (int k = k0; k < k1; k++) {
    // Update arc timings.
    TimingArcState &arcstate = getTimingArcState(timingSinkArcs[k]);

    arcstate.logicalEffortArrivalTime = netstate.logicalEffortArrivalTime;
  }  // end for
}  // end method

// -----------------------------------------------------------------------------

void Circuit::logicalEffort_UpdateTiming_Net(const int i) {
  EdgeArray<double> maxArrivalTime(-numeric_limits<double>::max(),
                                   -numeric_limits<double>::max());

  const TimingNet &net = timingNets[i];
  TimingNetState &netstate = getTimingNetState(i);

  const int k0 = timingArcPointers[i];
  const int k1 = timingArcPointers[i + 1];
  for (int k = k0; k < k1; k++) {
    // Update arc timings.
    const TimingArc &arc = timingArcs[k];
    TimingArcState &arcstate = getTimingArcState(k);

    const LibParserTimingInfo &timingInfo =
        arc.cell->actualInstType->timingArcs[arc.lut];

    const LibCellCharacterization &cellChar =
        getLibCellCharacterization(arc.cell);
    const LibArcCharacterization &arcChar = cellChar.arcs[arc.lut];

    assert(arc.sink == i);

    arcstate.logicalEffortDelay =
        arcChar.p + arcChar.g * (logicalEffort_GetGain(netstate, arcstate));

    // We hope the compiler will unroll this loop for us :)
    for (int outputEdge = 0; outputEdge < 2; outputEdge++) {
      const EdgeType inputEdge = ((EdgeType)outputEdge).getReversed();

      // set worst output arrival time
      const double arrivalTime = arcstate.logicalEffortArrivalTime[inputEdge] +
                                 arcstate.logicalEffortDelay[outputEdge];
      if (arrivalTime > maxArrivalTime[outputEdge]) {
        maxArrivalTime[outputEdge] = arrivalTime;
      }  // end if
    }    // end for
  }      // end for

  netstate.logicalEffortArrivalTime = maxArrivalTime;

  logicalEffort_UpdateTiming_Net_Endpoint(i);
}  // end method

// -----------------------------------------------------------------------------

void Circuit::logicalEffort_UpdateTiming() {
  const int numNets = timingNets.size();

  // For dummies net just set the logical effort to the pre-defined arrival
  // time.
  for (int i = 0; i < timingNumDummyNets; i++) {
    TimingNetState &netstate = getTimingNetState(i);
    netstate.logicalEffortArrivalTime = netstate.arrivalTime;
    logicalEffort_UpdateTiming_Net_Endpoint(i);
  }  // end for

  for (int i = timingNumDummyNets; i < numNets; i++) {
    logicalEffort_UpdateTiming_Net(i);
  }  // end for

  logicalEffort_UpdateTiming_Wns();
}  // end for

// -----------------------------------------------------------------------------

void Circuit::logicalEffort_Discretize_NetDriver(const int n) {
  const TimingNet &net = timingNets[n];
  const TimingNetState &netstate = getTimingNetState(n);

  if (!net.driver) {
    cout << "[WARNING] Trying to resize a null cell...\n";
  } else if (net.driver->dontTouch) {
    cout << "[WARNING] Trying to resize a don't touch cell...\n";
  } else {
    const int k = netstate.logicalEffortArrivalTime[RISE] >
                          netstate.logicalEffortArrivalTime[FALL]
                      ? netstate.logicalEffortBacktrack[RISE]
                      : netstate.logicalEffortBacktrack[FALL];

    const TimingArc &arc = timingArcs[k];
    const TimingArcState &arcstate = getTimingArcState(k);

    const LibCellGroupCharacterization &groupChar =
        getLibCellGroupCharacterization(arc.cell);
    const LibCellCharacterization &cellChar =
        getLibCellCharacterization(arc.cell);

    const int vth = cellChar.vth;
    const double expectedCin = arcstate.logicalEffortCin;

    int bestType = arc.cell->actualInstTypeIndex;
    int bestSize = -1;
    double bestError = +numeric_limits<double>::max();

    const double originalLoadViolation = getLoadViolation(arc.cell);

    for (int size = 0;
         size < groupChar.mappingVthSizeToCellTypeIndex[vth].size(); size++) {
      const int type = groupChar.mappingVthSizeToCellTypeIndex[vth][size];
      const double cin =
          getLibPinCapacitance(arc.cell->footprintIndex, type, arc.pin);

      const double error = std::abs(cin - expectedCin);
      const double loadViolation = getLoadViolationForCellType(arc.cell, type);

      if (error < bestError && loadViolation <= originalLoadViolation) {
        bestError = error;
        bestType = type;
        bestSize = size;
      }  // end if
    }    // end for

    updateCellType(arc.cell, bestType);

    if (enableReport_LogicalEffortDiscretization) {
      reportFile_LogicalEffortDiscretization
          << "Resizing cell " << arc.cell->instName << " from " << cellChar.size
          << " to " << bestSize << " expectedCin=" << expectedCin
          << " actualCin="
          << getLibPinCapacitance(arc.cell->footprintIndex, bestType, arc.pin)
          << " error= " << bestError << " fanout=" << net.fanout
          << " load=" << arc.cell->actualLoad
          << " load_violation=" << getLoadViolation(arc.cell) << "\n";
    }  // end if
  }    // end else

}  // end method

// -----------------------------------------------------------------------------

void Circuit::logicalEffort_Discretize() {
  const int numNets = timingNets.size();
  for (int n = numNets; n >= timingOffsetToNetLevel[1];
       n--) {  // skip dummy and nets driven by ffs.
    logicalEffort_Discretize_NetDriver(n);
  }  // end for
}  // end method

// -----------------------------------------------------------------------------

void Circuit::logicalEffort_Test() {
  cout << "worst delay (timer): " << timingWorstArrivalTime << "\n";
  cout << "reference slew: " << referenceSlew << "\n";

  logicalEffort_Reset();

  cout << "worst delay (logical effort): " << logicalEffortWorstArrivalTime
       << "\n";

  ofstream lefile("le.report");
  computeLogicalEffort_Report(lefile);

  for (int i = 0; i < 10; i++) {
    logicalEffort_Propagate();
    logicalEffort_Sizing();

    logicalEffort_UpdateTiming();
    std::cout << "worst arrival (timing): " << logicalEffortWorstArrivalTime
              << "\n";
  }  // end for

  logicalEffort_Discretize();

  updateTiming();
  logicalEffort_Reset();
  logicalEffort_UpdateTiming();
  logicalEffort_Report_Delays();
  logicalEffort_Report_ArrivaTimes();

}  // end method

//------------------------------------------------------------------------------
// Delay Scaling
//------------------------------------------------------------------------------

int Circuit::updateDelayScalingFactors_PathTrace(const int nstart) {
  EdgeArray<double> a;
  EdgeArray<double> q;
  EdgeArray<double> slack;
  EdgeType edgeType;
  int nstop;

  a = getTimingNetState(nstart).arrivalTime;
  q = timingRequiredTime[nstart];
  slack = q - a;

  const double slackThreshold = slack.getMin();

  edgeType = slack[RISE] < slack[FALL] ? RISE : FALL;
  nstop = nstart;

  while (nstop >= timingNumDummyNets) {
    const TimingNetState &netstate = getTimingNetState(nstop);
    a = netstate.arrivalTime;
    q = timingRequiredTime[nstop];
    slack = q - a;

    if (!approximatelyEqual(slack.getMin(), slackThreshold)) break;

    const TimingArc &arc = timingArcs[netstate.backtrack[edgeType]];

    nstop = arc.driver;
    edgeType.reverse();
  }  // end while

  return nstop;
}  // end method

// -----------------------------------------------------------------------------

void Circuit::updateDelayScalingFactors() {
  // [TODO] Not complete!

  const double T = getT();

  const int numNets = timingNets.size();
  for (int i = numNets - 1; i >= timingNumDummyNets; i--) {
    const int nstop = updateDelayScalingFactors_PathTrace(i);
  }  // end for

}  // end method

//------------------------------------------------------------------------------
// Analysis
//------------------------------------------------------------------------------

void Circuit::analysis_FanoutDistribution() {
  vector<int> numCellsPerFanout;
  int counter = 0;

  const int numCells = depthSortedCells.size();
  for (int k = offsetCombinational; k < numCells; k++) {
    Vcell *cell = depthSortedCells[k];

    const TimingNet &net = timingNets[cell->sinkNetIndex];
    if (net.fanout >= numCellsPerFanout.size()) {
      numCellsPerFanout.resize(net.fanout + 1, 0);
    }  // end if
    numCellsPerFanout[net.fanout]++;
    counter++;
  }  // end for

  const int numBins = numCellsPerFanout.size();
  for (int i = 0; i < numBins; i++) {
    cout << setw(2) << i << " " << numCellsPerFanout[i] << " "
         << "(" << ((numCellsPerFanout[i] / double(counter)) * 100) << "%)\n";
  }  // end for

}  // end method

//------------------------------------------------------------------------------

void Circuit::analysis_Cells() {
  ofstream file("report-cells.dat");

  const int numCells = depthSortedCells.size();
  for (int k = offsetCombinational; k < numCells; k++) {
    Vcell *cell = depthSortedCells[k];
    const TimingNet &net = timingNets[cell->sinkNetIndex];
    const LibCellCharacterization &cellChar = getLibCellCharacterization(cell);

    file << setw(25) << cell->instName << " " << setw(10) << cell->instType
         << " "
         << "size=" << cellChar.size << " "
         << "vth=" << cellChar.vth << " "
         << "fanout=" << setw(3) << net.fanout << " "
         << "load=" << cell->actualLoad << " "
         << "ipaths=" << net.ipaths << " "
         << "opaths=" << net.opaths << " "
         << "paths=" << (net.ipaths * net.opaths) << "\n";
  }  // end for
}  // end method

// -----------------------------------------------------------------------------

void Circuit::dumpVtStats(const std::string &filename) {
  std::ofstream out(filename);

  static const std::string VTs[3] = {"HVT", "SVT", "LVT"};

  out << std::fixed << std::setprecision(5);
  out
      //<< std::setw(7) << "Id" << ", "
      << std::setw(5) << "Type"
      << ", "
      //<< std::setw(7) << "Size" << ", "
      << std::setw(7) << "Fanout"
      << ", " << std::setw(12) << "Centrality"
      << ", " << std::setw(7) << "iDepth"
      << ", " << std::setw(7) << "oDepth"
      << ", " << std::setw(12) << "iPaths"
      << ", " << std::setw(12) << "oPaths"
      << "\n";

  int counter = 0;
  const int numCells = depthSortedCells.size();
  for (int k = offsetCombinational; k < numCells; k++) {
    Vcell *cell = depthSortedCells[k];
    const TimingNet &net = timingNets[cell->sinkNetIndex];
    const LibCellCharacterization &cellChar = getLibCellCharacterization(cell);

    out
        //<< std::setw(7) << counter << ", "
        << std::setw(4) << VTs[cellChar.vth]
        << ", "
        //<< std::setw(4) << cellChar.size << ", "
        << std::setw(6) << net.fanout << ", " << std::setw(12) << net.centrality
        << ", " << std::setw(6) << cell->logicalDepth << ", " << std::setw(6)
        << cell->reverseLogicalDepth << ", " << std::setw(12) << net.wipaths
        << ", " << std::setw(12) << net.wopaths << "\n";

    counter++;
  }  // end for
}  // end method
