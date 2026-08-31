set(MOD_NAME CoopPrototype)

set(SOURCE_FILES
	CMakeLists.txt
	CoopAreaStateJournal.cpp
	CoopAreaStateJournal.h
	CoopAreaObjectJournal.cpp
	CoopAreaObjectJournal.h
	CoopAreaStateOverlay.cpp
	CoopAreaStateOverlay.h
	CoopAnimationTest.cpp
	CoopBreakableSync.cpp
	CoopAssetDump.cpp
	CoopCoverageDiscovery.cpp
	CoopCoverageDiscovery.h
	CoopDamagePolicy.cpp
	CoopDamagePolicy.h
	CoopEnemyAuthorityPolicy.cpp
	CoopEnemyAuthorityPolicy.h
	CoopEnemyControlPolicy.cpp
	CoopEnemyControlPolicy.h
	CoopEnemyIntentGate.cpp
	CoopEnemyIntentGate.h
	CoopEnemyMovementGate.cpp
	CoopEnemyMovementGate.h
	CoopEnemyReplication.cpp
	CoopEnemyStateWire.cpp
	CoopEnemyStateWire.h
	CoopEntityLifecycle.cpp
	CoopLivePropSync.cpp
	CoopMultiplayerUi.cpp
	CoopPortraits.cpp
	CoopPortraitAssets.generated.h
	CoopHudPostFx.cpp
	CoopHazardSync.cpp
	CoopDialogueSync.cpp
	CoopTimeDilationSync.cpp
	CoopWorkstationSync.cpp
	CoopWorkstationSync.h
	CoopIdentityConfig.cpp
	CoopIdentityConfig.h
	CoopNativeSideBlob.cpp
	CoopNativeSideBlob.h
	CoopNetworkTelemetry.cpp
	CoopNetworkTelemetry.h
	CoopNativeEntityIdAllocator.cpp
	CoopNativeEntityIdAllocator.h
	CoopNativeFragmentApply.cpp
	CoopNativeFragmentApply.h
	CoopNativeFragmentPayload.cpp
	CoopNativeFragmentPayload.h
	CoopNativeFragmentImportPlanner.cpp
	CoopNativeFragmentImportPlanner.h
	CoopNativeFragmentMaterializer.cpp
	CoopNativeFragmentMaterializer.h
	CoopNativeFragmentResolver.cpp
	CoopNativeFragmentResolver.h
	CoopNativeGameStateOverlay.h
	CoopNativeGameStateFragmentLocator.cpp
	CoopNativeGameStateFragmentLocator.h
	CoopNativeInventoryBridge.cpp
	CoopNativePreloadRebuilder.cpp
	CoopNativePreloadRebuilder.h
	CoopPreloadSaveMerge.cpp
	CoopPreloadSaveMerge.h
	CoopNativeSaveBridge.cpp
	CoopNativeSaveBridge.h
	CoopNativeSavePackageRewriter.cpp
	CoopNativeSavePackageRewriter.h
	CoopNativeSaveStoreApi.cpp
	CoopNativeSaveStoreApi.h
	CoopNativeSaveXmlPatch.cpp
	CoopNativeSaveXmlPatch.h
	NullUi.cpp
	NullUi.h
    CoopPlayerSidecar.cpp
    CoopPtrHygiene.cpp
    CoopPtrHygiene.h
    CoopSharedDropSync.cpp
    CoopSharedStorageSync.cpp
	CoopPlayerState.cpp
	CoopPlayerFallRecovery.cpp
	CoopRuntimeExtractor.cpp
	CoopRuntimeExtractor.h
	CoopRuntimeGuards.cpp
	CoopRuntimeGuards.h
	CoopHookStats.cpp
	CoopHookStats.h
	CoopRuntimeLog.cpp
	CoopRuntimeLog.h
	CoopRuntimeConfig.cpp
	CoopRuntimeConfig.h
	CoopFilesystem.h
	CoopSerializerTrace.h
	CoopSaveStoreDecoder.cpp
	CoopSaveStoreDecoder.h
	CoopSaveStateBridge.cpp
	CoopSaveStateBridge.h
	CoopEventRouter.cpp
	CoopEventRouter.h
	CoopProtocol.h
	CoopVTableHook.h
	ModMain.cpp
	ModMain.h
	pch.h
)

add_library(${MOD_NAME} SHARED ${SOURCE_FILES})
target_link_libraries(${MOD_NAME}
	CommonMod
	ws2_32
)
target_link_options(${MOD_NAME} PRIVATE "/MAP:$<TARGET_FILE_DIR:${MOD_NAME}>/$<TARGET_FILE_BASE_NAME:${MOD_NAME}>.map")

target_precompile_headers(${MOD_NAME} PRIVATE pch.h)
mod_output_dir(${MOD_NAME} MOD)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCE_FILES})

# Compile and link a tiny executable that validates the engine-facing turret ABI.
# Keeping it outside SOURCE_FILES prevents main() from entering the mod DLL.
add_executable(CoopAbiSmokeTest EXCLUDE_FROM_ALL CoopAbiSmokeTest.cpp)
target_link_libraries(CoopAbiSmokeTest PRIVATE CommonMod advapi32)
set_target_properties(CoopAbiSmokeTest PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/abi-smoke"
)
add_dependencies(${MOD_NAME} CoopAbiSmokeTest)

add_executable(CoopWineFocusCtl CoopWineFocusCtl.cpp)
target_link_libraries(CoopWineFocusCtl user32 gdi32)
