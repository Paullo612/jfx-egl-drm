# jfx-egl-drm

## What is this?
This is OpenJFX Monocle EGL backend library based on Linux DRM. It uses atomic KMS API to provide EGL for ES2 Prism
backend.

## Building

### Ubuntu and Debian based distros

First of all, some development packages should be installed. This library requires `pkg-config`, `drm`, `gbm` and `egl`
development packages. JDK is also required for `jni.h`. CMake 3.24 is also required. And, of course, you'll have to have
working C compiler.

```console
user@ubuntu:~# sudo apt install gcc pkg-config libdrm-dev libgbm-dev libegl1-mesa-dev openjdk-17-jdk cmake make
```

Then you can build like that:
1. Create build directory
   ```console
   user@ubuntu:~# mkdir build && cd $_ 
   ```
2. Configure build
   ```console
   user@ubuntu:~# JAVA_HOME=/usr/lib/jvm/java-17-openjdk-arm64/ cmake -DBUILD_SHARED_LIBS=ON ../ 
   ```
   You can add `-DPRE_MULTIPLY_CURSOR=ON` option if your cursor plane has `pixel blend mode` property set to
   `Pre-multiplied` by default. You can also configure display scale factor by `SCALE_FACTOR` option, it takes float
   number as input (`-DSCALE_FACTOR="1.75"`, for example).
3. Build
   ```console
   user@ubuntu:~# make
   ```

## Launching

This library uses DRM directly, so, first of all, you should stop X server, or Wayland compositor, or Display Manager,
or anything that uses DRM exclusively. 

This is Monocle EGL backend, so, `glass.platform` should be `Monocle` and `monocle.platform` should be `EGL`. `use.egl`
should be set to `true` to tell OpenJFX that it should do a window composition. Also, you have to specify path to just
build library using `monocle.egl.lib` property: 
```console
root@ubuntu:~# java -Dglass.platform=Monocle \
                    -Dmonocle.platform=EGL \
                    -Dmonocle.egl.lib=/path/to/libjfx-egl-drm.so \
                    -Duse.egl=true \
                    -jar your-app.jar
```

By default, Monocle EGL uses `/dev/dri/card1` as display id. This can be changed by adding
`-Degl.displayid=/dev/dri/cardN` property, where N is the node of your video output controller.