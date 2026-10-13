#include "graphics.h"
#include "graphics_packets.h"
#include "runtime.h"
#include "hle/rt64_interpreter.h"
#include "gbi/rt64_gbi_f3d.h"
#include <array>
#include <cstring>

static size_t renderer_ram_size;
static uint64_t *triangle_counter;
static void goldeneyeRdpHalf1(RT64::State *state,RT64::DisplayList **dl) {
    size_t wordCount=GoldenEyePackets::triangleWords((*dl)->w1);
    if(!wordCount){RT64::GBI_F3D::rdpHalf1(state,dl);return;}
    uintptr_t pointer=reinterpret_cast<uintptr_t>(*dl),base=reinterpret_cast<uintptr_t>(state->RDRAM);
    size_t bytes=wordCount*sizeof(RT64::DisplayList);
    if(!triangle_counter || pointer<base || pointer-base>renderer_ram_size || bytes>renderer_ram_size-(pointer-base))
        recomp_fail("GoldenEye RDP triangle packet outside RAM",uint32_t(pointer-base));
    std::array<GoldenEyePackets::Command,GoldenEyePackets::maxTriangleWords> stream{};
    std::memcpy(stream.data(),*dl,bytes);
    std::array<uint32_t,GoldenEyePackets::maxTriangleWords> words{};
    size_t unpacked=0;
    if(!GoldenEyePackets::unpackTriangle(stream.data(),wordCount,words.data(),words.size(),unpacked))
        recomp_fail("GoldenEye malformed RDP triangle half commands",uint32_t(pointer-base));
    GoldenEyePackets::Mesh mesh;
    if(!GoldenEyePackets::makeMesh(words.data(),unpacked,state->rdp->otherMode.textPersp()==G_TP_PERSP,mesh))
        recomp_fail("GoldenEye invalid RDP triangle edges/coefficients",uint32_t(pointer-base));
    if(mesh.triangles)state->rdp->drawTris(mesh.triangles,mesh.position.data(),mesh.texcoord.data(),mesh.color.data(),
                                       (words[0]>>16)&7,(words[0]>>19)&7);
    *dl+=unpacked-1;
    (*triangle_counter)++;
}

void goldeneye_configure_graphics(void *opaque,size_t ram_size,uint64_t *rdp_triangles) {
    auto *interpreter=static_cast<RT64::Interpreter *>(opaque);
    renderer_ram_size=ram_size;triangle_counter=rdp_triangles;
    if(interpreter->hleGBI->ucode==RT64::GBIUCode::F3DGOLDEN)
        interpreter->hleGBI->map[F3D_G_RDPHALF_1]=goldeneyeRdpHalf1;
}
