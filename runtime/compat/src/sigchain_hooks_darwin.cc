// andropkg: bionic's optional hook consulted by ART's sigchain (weakly referenced and
// checked for nullptr there). Mach-O links need a definition; "no handler" == false.
#include <signal.h>

extern "C" bool android_handle_signal(int, siginfo_t*, void*) { return false; }
