LibYAML 0.2.x, MIT license: https://github.com/yaml/libyaml

The system development package supplies yaml.h and libyaml; no parser source
is vendored. runtime/config_document.cpp consumes the event API and resolves
scalars using the YAML 1.2 core schema. macOS bundles copy the linked library
and this license into the application. Linux builds require libyaml-dev.
