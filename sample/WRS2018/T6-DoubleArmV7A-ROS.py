import WRSUtil
WRSUtil.loadProject(
    "SingleSceneView", "T6", "AGXSimulator", "DoubleArmV7A",
    enableVisionSimulation = True, targetVisionSensors = "FRAME_FRONT_CAMERA", remoteType = "ROS")
