#!/usr/bin/env bash
# copy_step.sh ── 作り直しの「写す」手順（docs/study/10_rebuild.md の段 1〜3）。Git Bash で回す。
#   使い方:  bash copy_step.sh <段 1|2|3> <作り直しの根>
#   例:      bash docs/study/rebuild/copy_step.sh 1 /c/dev/AcousticFlowRebuild
#   写す元は既定でこのリポジトリ（AF_SRC で変えられる）。写すのは「今動いている物」だけで、旧コアは入れない。
#   ★1 つの段を写したら、必ずその段の検査を通してから次へ（どこで壊したかが 1 段に絞れる）。
set -e
SRC=${AF_SRC:-/c/dev/AcousticFlow}
step=${1:?段（1〜3）}
DST=${2:?作り直しの根}
cp_rel() { for f in "$@"; do mkdir -p "$DST/$(dirname "$f")"; cp "$SRC/$f" "$DST/$f"; done; }

case "$step" in
1)  # 段 1: 音を作る部品（DSP）とその検査。DSP は Core も Flow も知らない（下の層から作る）
    cp_rel AcousticEngine/src/Dsp/fft.h \
           AcousticEngine/src/Dsp/partitioned_convolver.h AcousticEngine/src/Dsp/nonuniform_convolver.h \
           AcousticEngine/src/Dsp/hrtf_set.h AcousticEngine/src/Dsp/hrtf_processor.h \
           AcousticEngine/src/Dsp/early_reflect_conv.h AcousticEngine/src/Dsp/direction_bus.h \
           AcousticEngine/src/Dsp/fdn_tail.h AcousticEngine/src/Dsp/fdn_room_mix.h \
           AcousticEngine/src/Dsp/reverb_tail_ir.h AcousticEngine/src/Dsp/tail_bus.h \
           AcousticEngine/src/Dsp/voice_renderer.h \
           AcousticEngine/src/Debug/detectors.h \
           AcousticEngineTest/dsp_regression.cpp
    ;;
2)  # 段 2: 形と部屋（Core）、音の届き方と配分（Flow）、GPU、とその検査
    cp_rel AcousticEngine/src/Core/vec3.h AcousticEngine/src/Core/aabb.h \
           AcousticEngine/src/Core/material.h AcousticEngine/src/Core/material.cpp \
           AcousticEngine/src/Core/worker_pool.h AcousticEngine/src/Core/room_graph.h AcousticEngine/src/Core/maekawa.h
    cp_rel AcousticEngine/src/Flow/world_rules.h AcousticEngine/src/Flow/emitter.h \
           AcousticEngine/src/Flow/surfaces.h AcousticEngine/src/Flow/surface_bvh.h AcousticEngine/src/Flow/trace_scene.h \
           AcousticEngine/src/Flow/probe.h AcousticEngine/src/Flow/budget.h AcousticEngine/src/Flow/energy_trace.h \
           AcousticEngine/src/Flow/aperture.h AcousticEngine/src/Flow/diffraction.h \
           AcousticEngine/src/Flow/image_sources.h AcousticEngine/src/Flow/image_surface.h AcousticEngine/src/Flow/image_lattice.h \
           AcousticEngine/src/Flow/receiver_layout.h AcousticEngine/src/Flow/receiver.h \
           AcousticEngine/src/Flow/mix.h AcousticEngine/src/Flow/response.h AcousticEngine/src/Flow/distribute.h \
           AcousticEngine/src/Flow/mix_to_voice.h AcousticEngine/src/Flow/world.h
    cp_rel AcousticEngine/src/Gpu/compute_d3d11.h AcousticEngine/src/Gpu/compute_d3d11.cpp \
           AcousticEngine/src/Gpu/trace_kernel.h AcousticEngine/src/Gpu/trace_gpu.h
    cp_rel AcousticEngineTest/flow_regression.cpp AcousticEngineTest/test_instruments.h
    ;;
3)  # 段 3: C の窓口（ヘッダ 4 枚と .cpp 3 本）と、台帳の検査。CMake は docs/study/rebuild/AcousticEngine/CMakeLists.txt
    cp_rel AcousticEngine/include/acoustic_scene.h AcousticEngine/include/acoustic_voice.h \
           AcousticEngine/include/acoustic_world.h AcousticEngine/include/acoustic_host.h \
           AcousticEngine/src/Export/world_api.cpp AcousticEngine/src/Export/voice_api.cpp AcousticEngine/src/Export/host_api.cpp \
           AcousticEngineTest/host_regression.cpp
    ;;
*)  echo "段は 1〜3"; exit 1 ;;
esac
echo "段 $step を写しました → $DST"
