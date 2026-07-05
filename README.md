# Audio Device Manager

A cross-plattform easy-to-use library for querying and controlling audio device settings.

## TODO
- [ ] create / split backend tests
  - one file for each backend for backend specific tests based on available features
  - one file for general tests, e.g. ensuring only one backend is loaded (i.e. ALSA fallback)
- [ ] add device feature flags, so the user can query if a device / backend support e.g. default devices
- [ ] unify more common functions in the abstract base class `AudioBackend`
  - mandatory common worker thread
  - every function called from outside (e.g. for device refresh or to set volume) should be called a request and be sent to the worker thread
  - maybe a common handle_request(fn) function that has callback to specific handlers but also manages promises/futures and on_done callbacks
  - abstract functions that are executed on the worker thread (e.g. `handle_set_volume` or `handle_device_refresh`)
- [ ] add poll thread for alsa backend to detect os audio device changes

## Notes
- for the time being do not implement pipewire backend, not needed due to pipewire-pulse compatibility layer
- prefer pulseaudio over alsa, because pulseaudio might have blocking hardware accesses and therefore alsa might not work for all devices
- prefer pulseaudio over pipewire, because tests show only partial volume control in pipewire (maybe can only set volume on application level, querying is fine)
