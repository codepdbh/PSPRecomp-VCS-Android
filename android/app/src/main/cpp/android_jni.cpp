#include "psprecomp/guest_memory.hpp"

#include <jni.h>

#include <string>

extern "C" JNIEXPORT jstring JNICALL
Java_com_psprecomp_vcs_MainActivity_nativeStatus(JNIEnv *env, jclass) {
    psprecomp::GuestMemory memory(32u * 1024u * 1024u);
    memory.store32(psprecomp::GuestMemory::kPhysicalBase, 0x56435331u);
    const bool core_linked =
        memory.load32(psprecomp::GuestMemory::kPhysicalBase) == 0x56435331u;
    const std::string status = core_linked
        ? "VCS Android: ARM64 core loaded.\n\nThis is the Android bring-up shell; game rendering, audio, storage and touch input are not connected yet."
        : "VCS Android: core initialization failed.";
    return env->NewStringUTF(status.c_str());
}
