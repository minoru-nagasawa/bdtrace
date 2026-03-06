#ifndef BDTRACE_TRACER_BACKEND_H
#define BDTRACE_TRACER_BACKEND_H

#include <string>
#include <vector>

namespace bdtrace {

class ITracerBackend {
public:
    virtual ~ITracerBackend() {}
    virtual int start(const std::vector<std::string>& argv) = 0;
    virtual int run_event_loop() = 0;
    virtual void stop() = 0;
};

} // namespace bdtrace

#endif // BDTRACE_TRACER_BACKEND_H
