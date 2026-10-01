#ifndef CODESYNTH_VM_CTX_H
#define CODESYNTH_VM_CTX_H

// A CodeSynth VM's user_data: the voice's sample cursor for smp() and the screen
// for the drawing primitives. Both borrowed, either may be null (smp then reads
// silence; audio VMs have no screen). Must be set before every func_run on a VM
// with ctx-registered natives. Tags only, so neither header is pulled in.

struct RenderCtx;
struct song_smp_voice_t;

typedef struct CodeSynthVmCtx {
    struct song_smp_voice_t *smp;
    struct RenderCtx        *screen;
} CodeSynthVmCtx;

#endif // CODESYNTH_VM_CTX_H
