cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain/android.cmake -DANDROID_NDK=/opt/android-ndk-r10d/ -DANDROID_ABI=armeabi-v7a with NEON -DANDROID_NATIVE_API_LEVEL=19 -DANDROID_APK_API_LEVEL=19 -DANDROID_APK_RUN=1 ../
