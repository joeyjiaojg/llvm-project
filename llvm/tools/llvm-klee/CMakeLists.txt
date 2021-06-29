set(LLVM_LINK_COMPONENTS
  AllTargetsAsmParsers
  AllTargetsDescs
  AllTargetsInfos
  BinaryFormat
  Core
  Demangle
  Object
  Support
  TextAPI
  )

add_llvm_tool(llvm-klee
  llvm-klee.cpp

  DEPENDS
  intrinsics_gen
  )

install(PROGRAMS llvm-klee.sh
  DESTINATION bin
  )
