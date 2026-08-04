# third_party

Vendored third-party code, kept verbatim with original licenses.

- `macaron/Base64.h` — base64 codec by tomykaira (MIT, © 2016).
  https://github.com/tomykaira/Base64.h

Local patches (marked `y-cpp fix` in the source):
- `macaron/Base64.h`: Encode loop condition `i < in_len - 2` underflowed
  (size_t) for inputs shorter than 2 bytes, causing out-of-bounds writes;
  changed to `i + 2 < in_len`.
