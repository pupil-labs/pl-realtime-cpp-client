#include "LoggingUsageEnvironment.hh"
#include <stdio.h>

LoggingUsageEnvironment::LoggingUsageEnvironment(TaskScheduler& taskScheduler, LogCallback callback) : BasicUsageEnvironment(taskScheduler) {
	this->callback = callback;
	std::memset(buffer, 0, BUFFER_SIZE);
}

void LoggingUsageEnvironment::writeFormatted(const char* format, ...) {
	if (callback != NULL) {
		va_list args;
		va_start(args, format);
		int len = vsnprintf(NULL, 0, format, args); //does not include null terminator
		if (len > 0 && len < BUFFER_SIZE) { //process only not empty messages that can fit the buffer
			if (bufferOffset + len >= BUFFER_SIZE) { //flush buffer if remaining space is not sufficient
				bufferOffset = 0;
				callback(buffer);
			}
#ifdef __ANDROID__
			bufferOffset += vsprintf(buffer + bufferOffset, format, args);
#else
			bufferOffset += vsprintf_s(buffer + bufferOffset, BUFFER_SIZE - bufferOffset, format, args);
#endif
			if (buffer[bufferOffset - 1] == '\n') { //flush each line
				bufferOffset = 0;
				callback(buffer);
			}
		}
		va_end(args);
	}
}

LoggingUsageEnvironment* LoggingUsageEnvironment::createNew(TaskScheduler& taskScheduler, LogCallback callback) {
	LoggingUsageEnvironment* env = new LoggingUsageEnvironment(taskScheduler, callback);
	return env;
}

UsageEnvironment& LoggingUsageEnvironment::operator<<(char const* str) {
	if (str == NULL) str = "(NULL)"; // sanity check
	writeFormatted("%s", str);
	return *this;
}

UsageEnvironment& LoggingUsageEnvironment::operator<<(int i) {
	writeFormatted("%d", i);
	return *this;
}

UsageEnvironment& LoggingUsageEnvironment::operator<<(unsigned u) {
	writeFormatted("%u", u);
	return *this;
}

UsageEnvironment& LoggingUsageEnvironment::operator<<(double d) {
	writeFormatted("%f", d);
	return *this;
}

UsageEnvironment& LoggingUsageEnvironment::operator<<(void* p) {
	writeFormatted("%p", p);
	return *this;
}