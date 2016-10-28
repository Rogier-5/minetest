#ifndef _TIMER_INCLUDED_
#define _TIMER_INCLUDED_

#include <climits>
#if __cplusplus >= 201103L
#include <cstdint>
#else
#include <stdint.h>
#endif
#include <ctime>
#include <cmath>
#include <cstring>
#include "porting.h"

#ifndef INT64_MAX
#define INT64_MIN (1LL << 63)
#define INT64_MAX (~(INT64_MIN))
#endif

struct Stat
{
	const int32_t scale;
	int n;
	int64_t sum;
	int64_t sumsq;
	int64_t min;
	int64_t max;
	Stat(int32_t s) : scale(s), n(0), sum(0), sumsq(0), min(INT64_MAX), max(INT64_MIN) {}
	void record(int64_t value) {
		value = (value + (scale + 1)/2) / scale;
		n++;
		sum += value;
		sumsq += value * value;
		if (value < min) min = value;
		if (value > max) max = value;
	}
	double average() const { return (double) sum / n; }
	double stddev() const { return sqrt((sumsq - (sum * sum) / (double) n) / (n - 1)); }
	std::string report(double s = 1, int p = 2) const {
		char buffer[100];
		sprintf(buffer, "%10.*f ± %10.*f  (%10.*f .. %10.*f)  [n: %d]",
			p, average() / s, p, stddev() / s, p, min / s, p, max / s, n);
		return buffer;
	}
	void reset(void) { n = 0; sum = 0; sumsq = 0; min = INT64_MAX; max = INT64_MIN; }
};

struct TimeStat
{
	TimeStat(int32_t s) : clock(s), cpu(s) {}
	Stat clock;
	Stat cpu;
	void reset(void) { clock.reset(); cpu.reset(); }
};

class Timer
{
public:
#if defined(CLOCK_MONOTONIC) && defined(CLOCK_THREAD_CPUTIME_ID) && !defined(_WIN32)
	Timer(TimeStat &s) : m_stat(s) {
		clock_gettime(CLOCK_MONOTONIC, &m_clock0);
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &m_cpu0);
	}
	~Timer(void) {
		struct timespec cpu1;
		struct timespec clock1;
		clock_gettime(CLOCK_MONOTONIC, &clock1);
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu1);
		u64 dtime = (clock1.tv_sec - m_clock0.tv_sec) * 1000 * 1000 * 1000;
		dtime += clock1.tv_nsec - m_clock0.tv_nsec;
		m_stat.clock.record(dtime);
		dtime = (cpu1.tv_sec - m_cpu0.tv_sec) * 1000 * 1000 * 1000;
		dtime += cpu1.tv_nsec - m_cpu0.tv_nsec;
		m_stat.cpu.record(dtime);
	}
private:
	struct timespec m_cpu0;
	struct timespec m_clock0;
#else
	#warning Class timer is non-functional on this platform
	Timer(TimeStat &s) : m_stat(s) {}
	~Timer(void) {
		m_stat.cpu.record(100);
		m_stat.clock.record(100);
		}
	
#endif
private:
	TimeStat &m_stat;
};

#endif // _TIMER_INCLUDED_
