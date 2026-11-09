// Added by WhiteCloudOL to cancel queued HTTP requests after disconnection.
#pragma once

#include "breeze/scheduler.h"
#include "httplib.h"

namespace breeze {

inline bool wait_for_slot(GenScheduler & sched, const httplib::Request & req,
                          httplib::Response & res, int slots = 1) {
    if (sched.acquire(slots, req.is_connection_closed)) return true;
    res.status = 499;
    res.set_content("{\"detail\":\"Request cancelled while waiting.\"}", "application/json");
    return false;
}

} // namespace breeze
