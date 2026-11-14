# dirwarden
A small Linux daemon that watches one or more directories and automatically deletes files a set number of seconds after they appear.

## Usage

```
dirwarden -p <directory> [-p <directory> ...] -t <seconds> [-f]
```

## Options

| Flag | Meaning |
|------|---------|
| `-p PATH` | Directory to monitor (can be given multiple times) |
| `-t SEC`  | File deletion time in seconds |
| `-f`      | Run in foreground |
| `-h`      | Show help |
| `-v`      | Show version |

## Example

```
dirwarden -p /tmp/drop -t 60
```

Watch `/tmp/drop` and delete every file 60 seconds after it appears.
