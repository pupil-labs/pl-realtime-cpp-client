#pragma once

#include "BasicUsageEnvironment.hh"
#include "RTSPService.hh"

class LoggingUsageEnvironment : public BasicUsageEnvironment {
public:
	static LoggingUsageEnvironment* createNew(TaskScheduler& taskScheduler, LogCallback callback, void* userData);

	virtual UsageEnvironment& operator<<(char const* str);
	virtual UsageEnvironment& operator<<(int i);
	virtual UsageEnvironment& operator<<(unsigned u);
	virtual UsageEnvironment& operator<<(double d);
	virtual UsageEnvironment& operator<<(void* p);

protected:
	LoggingUsageEnvironment(TaskScheduler& taskScheduler, LogCallback callback, void* userData);
	virtual void writeFormatted(const char* format, ...);

	static constexpr size_t BUFFER_SIZE = 1024;
	char buffer[BUFFER_SIZE];
	size_t bufferOffset = 0;
	LogCallback callback = NULL;
	void* userData = NULL;
};