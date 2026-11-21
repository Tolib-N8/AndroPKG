// andropkg (macOS dev loop): dalvikvm runs the VM on a secondary thread and keeps the process
// main thread in CFRunLoopRun(), as the iOS host does with UIKit. Apple frameworks that deliver
// work through the main queue (AVFoundation's player, notifications) then behave as on iOS.
#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#include <stdlib.h>

int andropkg_dalvikvm_main_cxx(int argc, char** argv);  // AOSP dalvikvm.cc main(), renamed

namespace {
int gArgc;
char** gArgv;

void* RunVm(void*) {
  exit(andropkg_dalvikvm_main_cxx(gArgc, gArgv));
}
}  // namespace

int main(int argc, char** argv) {
  gArgc = argc;
  gArgv = argv;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 8 << 20);  // the main thread's default
  pthread_t vm;
  if (pthread_create(&vm, &attr, RunVm, nullptr) != 0) return 1;
  CFRunLoopRun();
  return 0;
}
