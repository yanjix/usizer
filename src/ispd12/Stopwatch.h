#ifndef STOPWATCH_H
#define STOPWATCH_H

// UFRGS - Guilherme Flach
// This class was based on http://pocoproject.org/.

#if _WIN32
#include <windows.h>

#include <ctime>
#else
#include <sys/time.h>
#include <sys/times.h>
#include <time.h>
#include <unistd.h>
#endif

#include <cmath>
#include <sstream>
using std::ostringstream;
#include <string>
using std::string;
#include <iomanip>
using std::setfill;
using std::setw;

class Stopwatch {
 private:
#if _WIN32

#if defined(_MSC_VER) || defined(_MSC_EXTENSIONS)
#define DELTA_EPOCH_IN_MICROSECS 11644473600000000Ui64
#else
#define DELTA_EPOCH_IN_MICROSECS 11644473600000000ULL
#endif

  struct timezone {
    int tz_minuteswest; /* minutes W of Greenwich */
    int tz_dsttime;     /* type of dst correction */
  };

  inline int gettimeofday(struct timeval *tv, struct timezone *tz) {
    FILETIME ft;
    unsigned __int64 tmpres = 0;
    static int tzflag;

    if (NULL != tv) {
      GetSystemTimeAsFileTime(&ft);

      tmpres |= ft.dwHighDateTime;
      tmpres <<= 32;
      tmpres |= ft.dwLowDateTime;

      /*converting file time to unix epoch*/
      tmpres -= DELTA_EPOCH_IN_MICROSECS;
      tmpres /= 10; /*convert into microseconds*/
      tv->tv_sec = (long)(tmpres / 1000000UL);
      tv->tv_usec = (long)(tmpres % 1000000UL);
    }

    if (NULL != tz) {
      if (!tzflag) {
        _tzset();
        tzflag++;
      }
      tz->tz_minuteswest = _timezone / 60;
      tz->tz_dsttime = _daylight;
    }

    return 0;
  }  // end function

#endif

  bool clsRunning;

  double clsElapsedTime;
  timeval clsCheckpoint;

  void update() { gettimeofday(&clsCheckpoint, NULL); }  // end method

  double computeElapsedTimeWithRespectToCurrentTime() const {
    timeval t;
    gettimeofday(&t, NULL);

    return (t.tv_sec - clsCheckpoint.tv_sec) +
           (t.tv_usec - clsCheckpoint.tv_usec) / 1000000.0;
  }  // end method

 public:
  Stopwatch() { reset(); }

  void reset() {
    clsElapsedTime = 0;
    clsRunning = false;
  }  // end method

  void start() {
    clsRunning = true;
    update();
  }  // end method

  void restart() {
    reset();
    start();
  }  // end method

  void stop() {
    clsRunning = false;
    clsElapsedTime += computeElapsedTimeWithRespectToCurrentTime();
  }  // end method

  double getElapsedTime() const {
    if (clsRunning)
      return computeElapsedTimeWithRespectToCurrentTime();
    else
      return clsElapsedTime;
  }  // end method

  string getFormattedTime() const {
    const double hours = getElapsedTime() / (60 * 60);
    const double minutes = (hours - floor(hours)) * 60;
    const double seconds = (minutes - floor(minutes)) * 60;

    const int hh = (int)hours;
    const int mm = (int)minutes;
    const int ss = (int)seconds;

    ostringstream oss;
    oss << setfill('0') << hh << ":" << setw(2) << mm << ":" << setw(2) << ss;

    return oss.str();
  }  // end method

  bool isRunning() const { return clsRunning; }

};  // end class

#endif
