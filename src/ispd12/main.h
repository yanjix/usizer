/*
 *  main_cl.h
 *  gr
 *
 *  Created by Tiago Reimann on 27/06/11.
 *  Copyright 2011 __MyCompanyName__. All rights reserved.
 *
 */

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>

using namespace std;

void flowIsvlsi13(string benchmarkName, string dirRoot);

void flow0(string benchmarkName, string dirRoot);
void flow1(string benchmarkName, string dirRoot);
void flow2(string benchmarkName, string dirRoot);
void flow3(string benchmarkName, string dirRoot);
void flow4(string benchmarkName, string dirRoot);
void flow5(string benchmarkName, string dirRoot);
void flow6(string benchmarkName, string dirRoot);

void flowTiago(string benchmarkName, string dirRoot);
void flowGraci(string benchmarkName, string dirRoot);
void flowFlach(string benchmarkName, string dirRoot);
void flowJohann(string benchmarkName, string dirRoot);

void report(string benchmarkName, string dirRoot, string solutionFile);
