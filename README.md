# Audio Device Manager

A cross-plattform easy-to-use library for querying and controlling audio device settings.

## TODO
- [ ] create / split backend tests
  - one file for each backend for backend specific tests based on available features
  - one file for general tests, e.g. ensuring only one backend is loaded (i.e. ALSA fallback)
- [ ] add device feature flags, so the user can query if a device / backend support e.g. default devices
  - use feature flags in tests to determine if a test should be successful or return `unsupported`
- [ ] let all backend functions that interact with the OS be called with a timeout (which should be configurable), i.e. all handle_* functions
  - implement globally for all backends in the abstract class
  - return `timeout` error code when timeout occurs, then try to cancle the worker job so that other calls can be handled
  - timeout should be low, as all calls should be handled fairly fast by the os

## Notes
- for the time being do not implement pipewire backend, not needed due to pipewire-pulse compatibility layer
- prefer pulseaudio over alsa, because pulseaudio might have blocking hardware accesses and therefore alsa might not work for all devices
- prefer pulseaudio over pipewire, because tests show only partial volume control in pipewire (maybe can only set volume on application level, querying is fine)
