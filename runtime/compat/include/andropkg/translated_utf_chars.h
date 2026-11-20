// andropkg: ScopedUtfChars whose c_str() is the Android path translated onto the iOS layout
// (see andropkg/paths.h). Only strings under an Android system partition change, so it can
// stand in for every ScopedUtfChars in a file of syscall wrappers (libcore.io.Linux).
#pragma once
#include <string.h>

#include <nativehelper/scoped_utf_chars.h>

#include "andropkg/paths.h"

namespace andropkg {

class TranslatedUtfChars {
 public:
  TranslatedUtfChars(JNIEnv* env, jstring s) : chars_(env, s) {
    const char* raw = chars_.c_str();
    translated_ = raw ? andropkg_translate_path(raw, buf_, sizeof buf_) : nullptr;
  }
  const char* c_str() const { return translated_; }
  size_t size() const { return translated_ ? strlen(translated_) : 0; }
  const char& operator[](size_t n) const { return translated_[n]; }

 private:
  ScopedUtfChars chars_;
  const char* translated_;
  char buf_[1024];

  TranslatedUtfChars(const TranslatedUtfChars&) = delete;
  void operator=(const TranslatedUtfChars&) = delete;
};

}  // namespace andropkg
