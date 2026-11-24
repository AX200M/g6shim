# project

## add Unknown function info

- The file forwarders' `GetFullPathNameW` keeps a path on a mounted drive in drive form and reads `X:name` as `X:\name`, assuming the console has no per-drive current directory.
