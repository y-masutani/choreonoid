if(ENABLE_ASSIMP AND ENABLE_GUI)
  link_directories(${ASSIMP_LIBRARY_DIRS})
  choreonoid_add_plugin(CnoidAssimpPlugin AssimpPlugin)
  target_link_libraries(CnoidAssimpPlugin CnoidBase CnoidAssimpSceneLoader)
endif()
