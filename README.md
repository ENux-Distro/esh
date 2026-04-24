# esh
esh is a super tiny shell built by the creator of ENux

## Why esh

- Its super tiny (32 kilobytes)
- Suitable for minimal systems

## How to install esh

- Firstly, verify you have these dependencies installed
```bash
gcc make coreutils git
```
- Git clone the repo, and change directory onto the cloned repo
```bash
git clone https://github.com/ENux-Distro/esh.git
cd $(pwd)/esh
```
- Build the shell
```bash
make
```
- Optionally, install it to your host
```
sudo make install
```
