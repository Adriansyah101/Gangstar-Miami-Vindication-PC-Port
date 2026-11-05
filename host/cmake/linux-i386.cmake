# 32-bit Linux build with the system gcc (Debian, Ubuntu: gcc-multilib): the game is i386 code and runs in this
# process.  CMakeLists.txt uses this file by itself on a Linux host; the linux presets name it too.
set(CMAKE_C_FLAGS_INIT "-m32")
set(CMAKE_ASM_FLAGS_INIT "-m32")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-m32")
set(CMAKE_LIBRARY_ARCHITECTURE i386-linux-gnu)
