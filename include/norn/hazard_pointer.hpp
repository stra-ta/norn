#pragma once

// Compatibility umbrella for the 0.1 hazard-pointer header.  The reusable
// domain and guard primitives now live in norn/hazard/, while the queue lives
// beside the other queue implementations.

#include "norn/hazard/domain.hpp"
#include "norn/hazard/pointer.hpp"
#include "norn/queue/mpsc_linked_queue.hpp"
