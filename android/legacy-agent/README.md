# Legacy Root Agent APK（Android 1.0–4.4）

给 Android 1.0–4.4（API 1–19）准备的极小 APK：

- `minSdkVersion 1`，无 Activity、无 Provider、无界面；
- 静态 ARM 可执行文件 `assets/phone_agent`（NDK r10e，`android-3`，non-PIE，`-static`）；
- 启动后解压到应用私有目录，优先用 `su` 启动，失败则回退到应用 uid；
- agent 固定监听 `127.0.0.1:32145`，桌面端直接 `adb forward`，不依赖旧系统没有的 `content` 命令；
- 桌面端通过广播传入配对码：
  ```bash
  adb shell am broadcast -a com.phonetoolbox.agent.START -e pair_code 123456
  ```
- 配对码、`agent.out`、`peers.txt` 都在 `/data/data/com.phonetoolbox.agent/files/`，不会写到日志。

## 构建 agent

前置：

1. Android NDK r10e；
2. 用 NDK r10e 构建的 libsodium 1.0.22 静态库（`--disable-shared --enable-static --disable-asm`）；
3. **交叉编译必须设置 `ac_cv_c_bigendian=no`**。否则 configure 会把 ARM 误判成 big-endian，SHA-256/SHA-512 和 CPace 全部算错，表现为配对 MAC 永远不匹配。

libsodium 参考构建：

```bash
TOOL=/path/to/android-ndk-r10e/toolchains/arm-linux-androideabi-4.9/prebuilt/linux-x86_64/bin
SYSROOT=/path/to/android-ndk-r10e/platforms/android-3/arch-arm
ac_cv_c_bigendian=no \
CC="$TOOL/arm-linux-androideabi-gcc --sysroot=$SYSROOT" \
CFLAGS="--sysroot=$SYSROOT -O2 -mthumb" \
LDFLAGS="--sysroot=$SYSROOT -static" \
./configure --host=arm-linux-androideabi \
  --prefix=/tmp/libsodium-android3 \
  --disable-shared --enable-static --disable-asm
make -j"$(nproc)" && make install
```

agent：

```bash
cmake -S agent -B agent/build-legacy -G "Unix Makefiles" \
  -DCMAKE_SYSTEM_NAME=Android -DCMAKE_SYSTEM_VERSION=3 \
  -DCMAKE_ANDROID_ARCH_ABI=armeabi \
  -DCMAKE_ANDROID_NDK=/path/to/android-ndk-r10e \
  -DCMAKE_C_COMPILER=$TOOL/arm-linux-androideabi-gcc \
  -DCMAKE_C_FLAGS="--sysroot=$SYSROOT -mthumb -O2" \
  -DCMAKE_EXE_LINKER_FLAGS="--sysroot=$SYSROOT -static" \
  -DAGENT_LEGACY_STATIC=ON -DBUILD_SHARED_LIBS=OFF \
  -DSODIUM_ROOT=/tmp/libsodium-android3
cmake --build agent/build-legacy -j"$(nproc)"
```

## 构建 APK

```bash
ANDROID_SDK_ROOT=/opt/android-sdk \
ANDROID_BUILD_TOOLS=/opt/android-sdk/build-tools/34.0.0 \
AGENT_BINARY=agent/build-legacy/phone_agent \
./android/legacy-agent/build.sh
```

默认使用 `android/root-agent/agent-release.jks` 和 `.jks-pass`，只签 v1（SHA-1），兼容 Android 1.0。产物：

```
android/legacy-agent/build/PhoneToolbox-Agent-legacy.apk
```

可用 `ANDROID_BUILD_TOOLS`、`LEGACY_ANDROID_JAR`、`AGENT_KEYSTORE`、
`AGENT_KEYSTORE_PASS_FILE` 覆盖工具和签名配置。

## 验证

```bash
adb install -r android/legacy-agent/build/PhoneToolbox-Agent-legacy.apk
adb shell am broadcast -a com.phonetoolbox.agent.START -e pair_code 123456
adb shell cat /data/data/com.phonetoolbox.agent/files/agent.out
# 应看到 PORT=32145 / PID / PUBKEY / PAIRED
```

已在 Android 1.0（API 1）和 Android 2.3.7（API 10）模拟器上验证启动与 CPace 配对。

## 限制

- 无 `su` 的非 root 设备：agent 以应用 uid 运行，监听和文件操作可用，但 `exec`/`push`/`pull` 没有 root 能力；
- 只有 `/proc/mtd` 的旧 MTD 设备可以列出分区，但按 `by-name` 刷写的链路不适用，刷写前必须确认目标设备路径；
- 没有 Provider，审计日志和配对状态通过桌面端的 root 通道读取，不能像现代 APK 那样用 `content query`。
