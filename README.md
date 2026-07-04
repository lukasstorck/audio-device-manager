# Audio Device Manager

A cross-plattform easy-to-use library for querying and controlling audio device settings.

## TODO
- [ ] add Windows (WASAPI) support
- [ ] create / split backend tests
  - one file for each backend for backend specific tests based on available features
  - one file for general tests, e.g. ensuring only one backend is loaded (i.e. ALSA fallback)
- [ ] add device feature flags, so the user can query if a device / backend support e.g. default devices

## Notes
- for the time being do not implement pipewire backend, not needed due to pipewire-pulse compatibility layer
- prefer pulseaudio over alsa, because pulseaudio might have blocking hardware accesses and therefore alsa might not work for all devices
- prefer pulseaudio over pipewire, because tests show only partial volume control in pipewire (maybe can only set volume on application level, querying is fine)
