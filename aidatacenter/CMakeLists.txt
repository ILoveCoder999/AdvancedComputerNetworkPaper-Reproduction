# ============================================================================
# scratch/aidatacenter/CMakeLists.txt
# ----------------------------------------------------------------------------
# 把本目录下每个 ns-3 仿真 *-sim.cc 各编成一个独立可执行文件。
# 共享头文件(cmp-common.h / nccl-collectives.h / energy-model.h /
# pfc-lossless.h / mesh-route-header.h / mesh-credit.h / link-credit.h)与这些
# .cc 同目录,用引号 #include "xxx.h" 会自动从本目录解析,无需在此显式列出。
#
# 适用:CMake 版 ns-3(3.36+,即用 ./ns3 而非 ./waf 的版本)。
# ns-3 的 scratch/CMakeLists.txt 会自动 add_subdirectory 任何带 CMakeLists.txt
# 的子目录(官方有 scratch/subdir、scratch/nested-subdir 例子),无需改 ns-3 本体。
#
# 编译 & 运行(在 ns-3 根目录):
#   ./ns3 configure      # 仅首次/新增目录后需要;之后 CONFIGURE_DEPENDS 会自动重配
#   ./ns3 build
#   ./ns3 run "aidatacenter/leaf-spine-sim --scenario=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps --K=72 --nLeaf=36"
#   ./ns3 run "aidatacenter/mesh3d-sim     --scenario=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps --queuePkts=256"
#   ./ns3 run "aidatacenter/rng-sim --topology=rng --pattern=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps --queuePkts=256"
#   (只想单独编一个: ./ns3 build aidatacenter_leaf-spine-sim)
#
# 注意:纯 C++ 单测 / 出图管线自检(test_*.cc、cmp_pipeline_check.cc、
# link_credit_test.cc、mesh4d_pipeline_check.cc)不依赖 ns-3,**已在下方 exclude 排除**,
# 不在此编译;直接用 g++ 即可,例如:
#   g++ -std=c++14 -O2 link_credit_test.cc    -o /tmp/lct && /tmp/lct
#   g++ -std=c++14 -O2 mesh4d_pipeline_check.cc -o /tmp/chk && /tmp/chk .
#   g++ -std=c++14 -O2 test_cmp_common.cc     -o /tmp/tcc && /tmp/tcc
# ============================================================================

# 目标名前缀:ns-3 约定 scratch 子目录可执行名为 scratch_<相对路径>_<文件名>,
# 运行时写作 "<相对路径>/<文件名>"(本目录即 "aidatacenter/<name>")。
set(aidcn_target_prefix scratch_)
file(RELATIVE_PATH aidcn_path ${PROJECT_SOURCE_DIR}/scratch ${CMAKE_CURRENT_SOURCE_DIR})
string(REPLACE "/" "_" aidcn_path ${aidcn_path})

# 收集本目录所有 .cc
file(GLOB aidcn_srcs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)

# 排除:纯 C++ 单测 / 工具(不依赖 ns-3,用 g++ 单独编)。
# !! 关键:本套新增的 link_credit_test.cc / mesh4d_pipeline_check.cc 也是纯 C++,
#    必须在此排除,否则 CMake 会把它们当成 ns-3 仿真去编(它们各自带 main,
#    且 link_credit_test.cc 根本不含 ns-3 头)。
set(aidcn_exclude
    test_energy.cc
    test_collectives.cc
    test_cmp_common.cc
    cmp_pipeline_check.cc
    link_credit_test.cc
    mesh4d_pipeline_check.cc
)

foreach(aidcn_src ${aidcn_srcs})
  get_filename_component(aidcn_fname ${aidcn_src} NAME)     # 如 leaf-spine-sim.cc
  get_filename_component(aidcn_stem  ${aidcn_src} NAME_WE)  # 如 leaf-spine-sim

  list(FIND aidcn_exclude ${aidcn_fname} aidcn_idx)
  if(aidcn_idx EQUAL -1)
    build_exec(
      EXECNAME ${aidcn_stem}
      EXECNAME_PREFIX ${aidcn_target_prefix}${aidcn_path}_
      SOURCE_FILES ${aidcn_src}
      LIBRARIES_TO_LINK ${ns3-libs} ${ns3-contrib-libs}
      EXECUTABLE_DIRECTORY_PATH ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${aidcn_path}/
    )
  endif()
endforeach()
