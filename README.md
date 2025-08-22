# Pipewire OSC Modules

The pipewire OSC modules provide a convenient way to send OSC messages
from and to pipewire.

## Pipewire OSC Source

### Configuration

```spajson
{
  net.port = 1337
  node.description = "OSC Output to control a filter chain"
  node.autoconnect = true
  node.streams = [
    {
      node.name = "Compressor Control OSC Source"
      port.alias = "control_1"
      target.object = "My Compressor"
      osc.path = "/cmp"
    },
    {
      node.name = "Equalizer Control OSC Source"
      node.description = "Overridden property"
      port.alias = "control_1"
      target.object = "My Equalizer"
      osc.path = "/eq"
    },
  ]
}
```

## Pipewire OSC Sink

## Dependencies

- pipewire
- liblo
- boost
