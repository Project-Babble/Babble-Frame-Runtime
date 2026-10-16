#pragma once

namespace babble::crash {

// Call this one time early in main to write a backtrace to the log on a fatal signal.
void install();

}
