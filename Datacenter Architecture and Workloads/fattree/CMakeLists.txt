# scratch/fattree/CMakeLists.txt
# ----------------------------------------------------------------------------
# Fat-Tree (k-port) 数据中心网络仿真
#
# 放置位置: ns-3 源码树下的  scratch/fattree/CMakeLists.txt
# 编译:     ./ns3 build fattree
# 运行:     ./ns3 run fattree
#   带参数:  ./ns3 run "fattree --k=4"
#
# 说明: scratch 下的子目录如果带有自己的 CMakeLists.txt，ns-3 顶层
#       scratch/CMakeLists.txt 会自动 add_subdirectory() 进来，
#       因此这里直接调用 ns-3 提供的 build_exec() 即可。
# ----------------------------------------------------------------------------

build_exec(
  # 生成的可执行文件名 -> 用 `./ns3 run fattree` 运行
  EXECNAME fattree

  # 主程序用到的源文件:
  #   - fat-tree-simulation.cc        : 含 main()，搭拓扑 / 跑流量 / 统计带宽
  #   - fat-tree-routing.cc           : FatTreeRouting，两层路由 + 流分类 + 流调度
  #   - fat-tree-central-scheduler.cc : CentralScheduler，论文 3.7 节中央流调度器
  SOURCE_FILES
    fat-tree-simulation.cc
    fat-tree-routing.cc
    fat-tree-central-scheduler.cc

  # 链接 simulation.cc 里 #include 的各模块
  LIBRARIES_TO_LINK
    ${libcore}
    ${libnetwork}
    ${libinternet}
    ${libapplications}
    ${libpoint-to-point}
    ${libflow-monitor}
  # 如果以后嫌列模块麻烦，可整行替换为下面这句（链接所有已启用模块）:
  #   ${ns3-libs} ${ns3-contrib-libs}

  EXECUTABLE_DIRECTORY_PATH ${CMAKE_OUTPUT_DIRECTORY}/scratch/fattree/
)

# 论文 Table 2 第一列的传统过载树基线 (独立可执行文件)
# 运行: ./ns3 run "fattree-tree --pattern=random"
build_exec(
  EXECNAME fattree-tree
  SOURCE_FILES
    tree-baseline.cc
  LIBRARIES_TO_LINK
    ${libcore}
    ${libnetwork}
    ${libinternet}
    ${libapplications}
    ${libpoint-to-point}
    ${libflow-monitor}
  EXECUTABLE_DIRECTORY_PATH ${CMAKE_OUTPUT_DIRECTORY}/scratch/fattree/
)
