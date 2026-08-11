<#
    build_test_kit.ps1 - extract + convert the curated XCOM 2 test asset kit.

    One command rebuilds every asset under assets/models/ from the SDK.

    Run:  .\tools\xcom2\build_test_kit.ps1
          .\tools\xcom2\build_test_kit.ps1 -Only cliff,rocks     # subset
          .\tools\xcom2\build_test_kit.ps1 -SkipExtract     # reuse workbench/xcom_raw

    LAYOUT: one folder per asset, and each folder is SELF-CONTAINED - its
    meshes, its textures and its .mtl, with nothing referenced across folders.
    That costs a little duplication (the cliffs and the boulders share a rock
    atlas and each keep a copy) and buys the ability to drop a single folder
    into the project without chasing dependencies. Categories are:

        cover/      shootable cover props
        vegetation/ plants and organic scatter
        ground/     man-made paving
        terrain/    natural ground, cliffs, rock

    Each entry below is one MATERIAL. Several entries may share an `out` folder
    when they belong to the same asset (e.g. the maple's canopy and bark).

    Texture budget: props capped at 512 (small objects under a tactical
    camera), terrain at 1024 where tiling resolution shows. -PropSize /
    -TerrainSize override.

    See assets/models/README.md for the inventory and the material gotchas.
#>
[CmdletBinding()]
param(
    [string[]]$Only,
    [switch]$SkipExtract,
    [string]$SdkRoot = 'E:\SteamLibrary\steamapps\common\XCOM 2 SDK',
    [int]$PropSize = 512,
    [int]$TerrainSize = 1024
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

# ---------------------------------------------------------------- the manifest
# meshes/textures are 'SourceName=OutputName'. Cover class and tile footprint
# are already encoded in XCOM's own mesh names (LoCov / HiCover / 1x1 / x2),
# which is why the output names can stay this literal.
#
# 'texpkg'/'tex2' pull textures from a DIFFERENT package than the meshes - the
# common case for ground art, whose materials live in shared TextureLibrary_*
# packages.
$kit = @(
  # ==================================================================== cover
  @{ id='cinderblock_wall'; pkg='CinderblockWallA'; out='cover/cinderblock_wall'; size=$TerrainSize
     meshes=@('CndrBlkWall_HiFence_x1_A=wall_hi_x1','CndrBlkWall_LoFence_x1_A=wall_lo_x1',
              'CndrBlkWall_Destro_x1_A=wall_hi_x1_destroyed')
     tex=@('CinderblockWallB_DIF=wall_dif','CinderblockWallB_NRM=wall_nrm',
           'CinderblockWallB_MSK=wall_msk') }

  @{ id='sandbags'; pkg='SandbagSet'; out='cover/sandbags'; size=$PropSize
     meshes=@('SandBagsStr_LoCov_1x1_A=sandbags_lo_x1','SandBagsStr_LoCov_2x1_A=sandbags_lo_x2',
              'SandBagsCrn_LoCov_1x1_A=sandbags_lo_corner')
     tex=@('Sandbag_DIF=sandbags_dif','Sandbag_NRM=sandbags_nrm','Sandbag_MSK=sandbags_msk') }

  @{ id='jersey'; pkg='JerseyBarrier'; out='cover/jersey_barrier'; size=$PropSize
     meshes=@('JerseyWall_LoCov_x1_A=jersey_lo_x1','JerseyWall_LoCov_x2_A=jersey_lo_x2')
     tex=@('JerseyBarrier_DIF=jersey_dif','JerseyBarrier_NRM=jersey_nrm','JerseyBarrier_MSK=jersey_msk') }

  @{ id='crate'; pkg='WoodenCratesA'; out='cover/crates'; size=$PropSize
     meshes=@('WoodenCratesA_HiCover_1x1_A=crate_hi_1x1','WoodenCratesA_LoCover_1x1_A=crate_lo_1x1',
              'WoodenCratesA_Debris_A=crate_debris')
     tex=@('WoodenCratesA_DIF=crate_dif','WoodenCratesA_NRM=crate_nrm','WoodenCratesA_MSK=crate_msk') }

  @{ id='barrel'; pkg='MetalBarrelA'; out='cover/barrel'; size=$PropSize
     meshes=@('MetalBarrelA_LoCov_1x1A=barrel_lo_1x1','MetalBarrelA_LoCov_1x1D=barrel_lo_1x1_b')
     tex=@('MetalBarrelA_A_DIF=barrel_dif','MetalBarrelA_A_NRM=barrel_nrm','MetalBarrelA_MSK=barrel_msk') }

  @{ id='guardrail'; pkg='GuardRails'; out='cover/guardrail'; size=$PropSize
     meshes=@('GuardRail_LoFenceStr_x2_A=guardrail_lo_x2','GuardRail_LoFenceCap_x2_A=guardrail_lo_cap')
     tex=@('GuardRails_DIF=guardrail_dif','GuardRails_NRM=guardrail_nrm','GuardRails_MSK=guardrail_msk') }

  @{ id='hydrant'; pkg='FireHydrantA'; out='cover/hydrant'; size=$PropSize
     meshes=@('FireHydrant_LoCover_1x1_A=hydrant_lo_1x1')
     tex=@('FireHydrantA_DIFF=hydrant_dif','FireHydrantA_NORM=hydrant_nrm','FireHydrantA_MSK=hydrant_msk') }

  @{ id='picnic'; pkg='PicnicTable'; out='cover/picnic_table'; size=$PropSize
     meshes=@('PicnicTableCombo=picnic_table','PicnicTableDestro=picnic_table_destroyed')
     tex=@('PicnicTableA_DIFF=picnic_dif','PicnicTableA_NORM=picnic_nrm','PicnicTableA_MSK=picnic_msk') }

  # --- more walls and fences, for variety against the cinderblock wall ---
  # BoundaryWallA is the only one here with a full-height (HiCover) run; the
  # rest are waist-high LoCov/LoFence, which is what most XCOM fencing is.
  @{ id='boundary_wall'; pkg='BoundaryWallA'; out='cover/boundary_wall'; size=$TerrainSize
     meshes=@('BoundaryWallA_HiCover_x3_A=boundary_hi_x3','BoundaryWallA_LoCover_x2_A=boundary_lo_x2',
              'BoundaryWallA_LoCover_x3_A=boundary_lo_x3')
     tex=@('BoundaryWallA_DIF=boundary_dif','BoundaryWallA_NRM=boundary_nrm',
           'BoundaryWallA_MSK=boundary_msk') }

  @{ id='stone_wall'; pkg='StoneWallA'; out='cover/stone_wall'; size=$TerrainSize
     meshes=@('StoneWallA_LoFenceStr_x1A=stone_lo_x1','StoneWallA_LoFenceStr_x2A=stone_lo_x2',
              'StoneWallA_Deco_x1A=stone_deco_x1')
     tex=@('StoneWallA_DIF=stone_dif','StoneWallA_NRM=stone_nrm','StoneWallA_MSK=stone_msk') }

  # The B texture set is the complete one (the unsuffixed set has no MSK).
  @{ id='brick_fence'; pkg='BrickFence'; out='cover/brick_fence'; size=$TerrainSize
     meshes=@('BrickFence_LoCov_x1_A=brick_lo_x1','BrickFence_LoCov_x2_A=brick_lo_x2',
              'BrickFence_DecoPost_A=brick_post')
     tex=@('BrickFence_B_DIF=brick_dif','BrickFence_B_NRM=brick_nrm','BrickFence_B_MSK=brick_msk') }

  @{ id='wooden_fence'; pkg='FenceWoodenA'; out='cover/wooden_fence'; size=$TerrainSize
     meshes=@('FenceWoodenA_LoFenceStr_x2_A=wood_fence_lo_x2',
              'FenceWoodenA_LoFenceCap_x2_A=wood_fence_lo_cap',
              'FenceWoodenA_LoFenceStr_x2_DestroA=wood_fence_destroyed')
     tex=@('FenceWoodenA_DIF=wood_fence_dif','FenceWoodenA_NRM=wood_fence_nrm',
           'FenceWoodenA_MSK=wood_fence_msk') }

  @{ id='privacy_fence'; pkg='PrivacyFence'; out='cover/privacy_fence'; size=$TerrainSize
     meshes=@('PrivacyFence_HiFenceStr_X2_C=privacy_hi_x2','PrivacyFence_HiFenceCap_X2_C=privacy_hi_cap',
              'PrivacyFence_HiFenceCap_X2_DestroA=privacy_hi_destroyed')
     tex=@('PrivacyFence_DIF=privacy_dif','PrivacyFence_NRM=privacy_nrm','PrivacyFence_MSK=privacy_msk') }

  # --- lamp posts. All three are destructible, hence the Destroyed/Chunk
  # siblings in their packages; one destroyed state each is taken here. ---
  @{ id='street_light'; pkg='TWN_StreetLight'; out='cover/street_light'; size=$PropSize
     meshes=@('TWN_StreetLight_HiCov_1x1_A=street_light_hi_1x1',
              'TWN_StreetLight_Destroyed=street_light_destroyed',
              'TWN_StreetLight_Deco_A=street_light_deco')
     tex=@('TWN_StreetLight_DIF=street_light_dif','TWN_StreetLight_NRM=street_light_nrm',
           'TWN_StreetLight_MSK=street_light_msk') }

  @{ id='park_lamp'; pkg='ParkLampA'; out='cover/park_lamp'; size=$PropSize
     meshes=@('ParkLampA_LoCov_x1A=park_lamp_lo_1x1','ParkLampA_Deco_A=park_lamp_deco')
     tex=@('ParkLampA_DIF=park_lamp_dif','ParkLampA_NRM=park_lamp_nrm','ParkLampA_MSK=park_lamp_msk') }

  # EMIS is the emissive lit-lamp mask - not a colour map, feed it to emission.
  @{ id='light_post'; pkg='CityCenterLightPostA'; out='cover/light_post'; size=$PropSize
     meshes=@('CityCenterLightPostA_1x1A=light_post_1x1','CityCenterLightPostA_1x1C=light_post_1x1_c',
              'CityCenterWallLight_1x1A=wall_light_1x1')
     tex=@('CityCenterLightPostA_DIF=light_post_dif','CityCenterLightPostA_NRM=light_post_nrm',
           'CityCenterLightPostA_MSK=light_post_msk','CityCenterLightPostA_EMIS=light_post_emis') }

  # ============================================================= interactive
  # Ladder mesh names are HEIGHTS IN UNREAL UNITS, and they land on the
  # lattice exactly: 192uu = kStoreyHeight (3 z-cells), 256 = 4 cells,
  # 512 = 8 cells. Ready-made cases for LadderQuery.
  @{ id='ladder_metal'; pkg='LadderMetal'; out='interactive/ladder_metal'; size=$PropSize
     meshes=@('LadderMetalA_192=ladder_metal_192','LadderMetalA_256=ladder_metal_256',
              'LadderMetalA_512=ladder_metal_512','LadderHatch_256=ladder_hatch_256')
     tex=@('LadderMetalA_DIF=ladder_metal_dif','LadderMetalA_NRM=ladder_metal_nrm',
           'LadderMetalA_MSK=ladder_metal_msk') }

  @{ id='ladder_wood'; pkg='LadderWood'; out='interactive/ladder_wood'; size=$PropSize
     meshes=@('LadderWoodA_192=ladder_wood_192','LadderWoodA_256=ladder_wood_256',
              'LadderWoodA_512=ladder_wood_512')
     tex=@('LadderWood_DIF=ladder_wood_dif','LadderWood_NRM=ladder_wood_nrm',
           'LadderWood_MSK=ladder_wood_msk') }

  # ================================================================ vehicles
  # Cars are full-cover, destructible, and ship their damage states as separate
  # meshes. Note the source typo "Minvan" on the minivan meshes.
  @{ id='sedan'; pkg='Sedan_C'; out='vehicles/sedan'; size=$PropSize
     meshes=@('Sedan_C=sedan','Sedan_C_Nodoors=sedan_nodoors','Sedan_C_Burned=sedan_burned',
              'Sedan_C_Door=sedan_door')
     tex=@('Sedan_C_DIF=sedan_dif','Sedan_C_NRM=sedan_nrm','Sedan_C_MSK=sedan_msk') }

  @{ id='minivan'; pkg='Minivan'; out='vehicles/minivan'; size=$PropSize
     meshes=@('Minvan_A=minivan','Minvan_A_DEST=minivan_destroyed')
     tex=@('Minivan_A_DIF=minivan_dif','Minivan_A_NRM=minivan_nrm','Minivan_A_MSK=minivan_msk') }

  @{ id='car_wreck'; pkg='GraveyardCars'; out='vehicles/car_wreck'; size=$PropSize
     meshes=@('GraveyardCarA=car_wreck','GraveyardCarA_Flipped=car_wreck_flipped')
     tex=@('GraveyardCarA_DIF=car_wreck_dif','GraveyardCarA_NRM=car_wreck_nrm',
           'GraveyardCarA_MSK=car_wreck_msk') }

  # =============================================================== vegetation
  # NOT CityCenterTree: every texture in that package is a flat 64x64 colour
  # swatch (a placeholder - the real art is here in Foliage_Temperate).
  #
  # The maple is the one MULTI-MATERIAL mesh in the kit: trunk and canopy are
  # separate material slots in the original, but UnrealEd's OBJ exporter merges
  # every section into one `g` group, so the split cannot be recovered from the
  # OBJ. Bark textures ship in the same folder for a hand-built trunk material.
  @{ id='maple'; pkg='Foliage_Temperate'; out='vegetation/maple'; size=$PropSize
     meshes=@('TWN_MapleTreeA_Small_A=maple_small','TWN_MapleTreeA_Med_A=maple_med',
              'TWN_MapleTreeA_Small_A_DESTRO=maple_small_destroyed')
     tex=@('MapleBranches_DIF=maple_branches_dif','MapleBranches_NRM=maple_branches_nrm',
           'MapleBranches_MSK=maple_branches_msk') }

  @{ id='maplebark'; pkg='Foliage_Temperate'; out='vegetation/maple'; size=$PropSize
     meshes=@()
     tex=@('MapleTreeBarkA_DIF=maple_bark_dif','MapleTreeBarkA_NRM=maple_bark_nrm',
           'MapleTreeBarkA_MSK=maple_bark_msk') }

  # The B-series bushes are the detailed ones. TemperateBush_Deco_1x1A is a
  # coarse 130-tri scatter proxy spanning 4.6 tiles - deliberately not used.
  @{ id='bush'; pkg='TemperateBushes'; out='vegetation/bush'; size=$PropSize
     meshes=@('TemperateBushB_Deco_1x1_A=bush_1x1','TemperateBushB_Deco_2x2_A=bush_2x2')
     tex=@('TemperateBush_Deco_A_DIFF=bush_dif','TemperateBush_Deco_A_NORM=bush_nrm',
           'TemperateBush_Deco_A_OpcMSK=bush_opacity') }

  @{ id='ferns'; pkg='TemperateBushes'; out='vegetation/ferns'; size=$PropSize
     meshes=@('TemperateFerns_1x1A=ferns_1x1','TemperateFerns_2x2A=ferns_2x2')
     tex=@('TemperatePlants_DIFF=ferns_dif','TemperatePlants_NORM=ferns_nrm',
           'TemperatePlants_OpcMsk=ferns_opacity') }

  # Grass CLUMPS - actual geometry, not a tiling texture. Single material.
  @{ id='grassclump'; pkg='Foliage_Temperate'; out='vegetation/grass_clump'; size=$PropSize
     meshes=@('WLD_GrassDecoA_x1A=grass_clump_1x1','WLD_GrassDecoA_2x2A=grass_clump_2x2',
              'WLD_GrassDecoATall_x1A=grass_clump_tall_1x1','WLD_WeedsDecoA_x1A=weeds_1x1')
     tex=@('WLD_GrassDecoA_DIF=grassclump_dif','WLD_GrassDecoA_NRM=grassclump_nrm',
           'WLD_GrassDecoA_MSK=grassclump_msk') }

  @{ id='cattail'; pkg='Foliage_Temperate'; out='vegetation/cattail'; size=$PropSize
     meshes=@('WLD_Cattail_01=cattail','WLD_Cattail_02=cattail_b')
     tex=@('WLD_Cattail_DIF=cattail_dif','WLD_Cattail_NRM=cattail_nrm',
           'WLD_Cattail_MSK=cattail_msk') }

  @{ id='vines'; pkg='WLD_Vines'; out='vegetation/vines'; size=$PropSize
     meshes=@('WLD_VineBranch_Deco_1x1A=vines_1x1','WLD_VineBranch_Deco_1x1B=vines_1x1_b')
     tex=@('WLD_VinesA_DIFF=vines_dif','WLD_VinesA_NORM=vines_nrm','WLD_VinesA_MSK=vines_msk') }

  # Fallen branches and twigs - what makes a forest floor read as a forest.
  @{ id='forest_scatter'; pkg='ForestScatterDeco'; out='vegetation/forest_scatter'; size=$PropSize
     meshes=@('ForestScatterDeco_A=forest_scatter_a','ForestScatterDeco_C=forest_scatter_c',
              'ForestScatterDeco_E=forest_scatter_e','ForestScatterDeco_G=forest_scatter_g')
     tex=@('ForestScatterDeco_DIF=forest_scatter_dif','ForestScatterDeco_NRM=forest_scatter_nrm',
           'ForestScatterDeco_MSK=forest_scatter_msk') }

  # Fallen logs and stumps - the forest's natural cover. TreeStumpCover carries
  # no textures of its own; it paints from Foliage_Temperate's maple bark, so
  # that is what is pulled in here.
  @{ id='stump'; pkg='TreeStumpCover'; out='vegetation/logs_stumps'; size=$PropSize
     meshes=@('TreeStump_HiCov_1x1_A=stump_hi_1x1','TreeStump_LoCov_1x1_A=stump_lo_1x1',
              'TreeLog_LoCov_1x2_A=log_lo_1x2','TreeLog_LoCov_1x3_A=log_lo_1x3',
              'TreeLog_HiCov_2x2_D=log_hi_2x2','TreeLog_DecoScatter_A=log_scatter')
     tex=@() ; texpkg='Foliage_Temperate'
     tex2=@('MapleTreeBarkA_DIF=bark_dif','MapleTreeBarkA_NRM=bark_nrm',
            'MapleTreeBarkA_MSK=bark_msk') }

  # =========================================================== ground (paved)
  @{ id='road'; pkg='SmallTown_RoadPlateA'; out='ground/road'; size=$TerrainSize
     meshes=@('TWN_RdPltStr_08x08_A=road_straight_8x8','TWN_RdPltCrn_08x08_A=road_corner_8x8',
              'TWN_RdPltInt_08x08_A=road_intersection_8x8')
     tex=@() ; texpkg='TextureLibrary_Asphault'
     tex2=@('AsphaltSmoothA_DIF=asphalt_dif','AsphaltSmoothA_NRM=asphalt_nrm','AsphaltA_MSK=asphalt_msk') }

  @{ id='sidewalk'; pkg='TWN_Sidewalk'; out='ground/sidewalk'; size=$TerrainSize
     meshes=@('Sidewalk_Str_x2_A=sidewalk_str_x2','Sidewalk_Str_x4_A=sidewalk_str_x4',
              'Sidewalk_Crn_2x2_A=sidewalk_corner_2x2','Sidewalk_Plt_4x4_A=sidewalk_plate_4x4',
              'Sidewalk_Raised_x9_A=sidewalk_raised_x9','Curb_Str_x4_A=curb_str_x4')
     tex=@() ; texpkg='TextureLibrary_Sidewalk'
     tex2=@('SidewalkTileA_DIF=sidewalk_dif','SidewalkTileA_NRM=sidewalk_nrm',
            'SidewalkTileA_MSK=sidewalk_msk',
            'SidewalkTileDamagedA_DIF=sidewalk_damaged_dif',
            'SidewalkTileDamagedA_NRM=sidewalk_damaged_nrm') }

  # ROAD MARKINGS. The road plates are deliberately bare asphalt - XCOM paints
  # every line, crosswalk and arrow with SEPARATE decal geometry laid over the
  # plate, from RoadDetails.upk. That is why road_straight_8x8 has no yellow
  # centre line: it never did. Two ways to use these:
  #   * the *_decal meshes are thin overlay strips to place on a plate;
  #   * road_yellow_* are pre-marked full-plate overlays sized to match the
  #     16x8 / 8x8 road plates outright.
  # Both alpha-cut from the MSK blue channel, so they need the same masked
  # material treatment as foliage, plus a depth bias to avoid z-fighting.
  @{ id='road_markings'; pkg='RoadDetails'; out='ground/road_markings'; size=$TerrainSize
     meshes=@('YellowStripeDecal_8x=yellow_stripe_8x','YellowStripeDecal_4x=yellow_stripe_4x',
              'YellowStripeDecal_1xA=yellow_stripe_1x','WhiteStripeDecal_8x=white_stripe_8x',
              'CrosswalkA=crosswalk','ArrowDecal=arrow',
              'TWN_RoadStraightYellow_16x8_A=road_yellow_straight_16x8',
              'TWN_RoadCornerYellow_8x8_A=road_yellow_corner_8x8',
              'TWN_IntersectionMarkings_A=intersection_markings')
     tex=@('RoadStripes_DIF=stripes_dif','RoadStripes_NRM=stripes_nrm','RoadStripes_MSK=stripes_msk') }

  # Wear decals from the same package but a different material: patches,
  # potholes, skidmarks, cracks.
  @{ id='road_wear'; pkg='RoadDetails'; out='ground/road_markings'; size=$TerrainSize
     meshes=@('PotholeDecal_A=pothole','SkidmarkA=skidmark','CrackedRoadDecal=cracked_road',
              'AsphaltPatch_Rect_A=asphalt_patch','TarStrip_8x=tar_strip_8x')
     # RoadDetails_MSK is omitted: it is empty (flat), unlike RoadStripes_MSK.
     # These wear decals alpha-cut from the DIFFUSE alpha instead.
     tex=@('RoadDetails_DIF=roadwear_dif','RoadDetails_NRM=roadwear_nrm') }

  @{ id='concrete'; pkg='TextureLibrary_Concrete'; out='ground/concrete'; size=$TerrainSize
     meshes=@()
     tex=@('ConcreteSmoothA_DIF=concrete_dif','ConcreteSmoothA_NRM=concrete_nrm',
           'ConcreteB_MSK=concrete_msk',
           'ConcreteWornB_DIF=concrete_worn_dif','ConcreteWornB_NRM=concrete_worn_nrm',
           'ConcreteCracksA_DIF=concrete_cracks_dif','ConcreteCracksA_NRM=concrete_cracks_nrm') }

  # ======================================================= terrain (natural)
  # Cliffs. TemperatePlateaus carries NO textures of its own - it paints with
  # the TemperateRocks atlases, so those are pulled in alongside.
  # TemperateRockLadder_256 is a climbable rock face: 256uu = 4 z-cells.
  @{ id='cliff'; pkg='TemperatePlateaus'; out='terrain/cliff'; size=$TerrainSize
     meshes=@('TemperatePlateau_A=cliff_a','TemperatePlateau_C=cliff_c','TemperatePlateau_F=cliff_f',
              'TemperatePlateau_LowCover_3x3=cliff_lo_3x3','TemperatePlateau_LowCover_6x6=cliff_lo_6x6',
              'TemperatePlatBoltOn_HiCover_1x1A=cliff_bolton_hi_1x1',
              'TemperatePlatBoltOn_LoCover_1x1A=cliff_bolton_lo_1x1',
              'TemperateRockLadder_256=rock_ladder_256')
     tex=@() ; texpkg='TemperateRocks'
     tex2=@('TemperateRockB2_DIFF=cliff_dif','TemperateRockB2_NORM=cliff_nrm',
            'TemperateRockC_DIFF=cliff_c_dif','TemperateRockC_NORM=cliff_c_nrm') }

  # Note the source typo "Temeprate" on the 1x1 deco rocks - it is in the SDK.
  @{ id='rocks'; pkg='TemperateRocks'; out='terrain/rocks'; size=$TerrainSize
     meshes=@('TemperateRock_HiCover_1x1A=rock_hi_1x1','TemperateRock_HiCover_2x2A=rock_hi_2x2',
              'TemeprateRock_Deco_1x1_A=rock_deco_1x1','TemperateRock_Deco_2x2_B=rock_deco_2x2',
              'TemperateRock_Deco_4x4_A=rock_deco_4x4')
     tex=@('TemperateRockA_DIFF=rock_dif','TemperateRockA_NORM=rock_nrm',
           'TemperateMossA_DIFF=moss_dif','TemperateMossA_NORM=moss_nrm') }

  @{ id='marsh'; pkg='TemperateMarshes'; out='terrain/marsh'; size=$TerrainSize
     meshes=@('TemperateMarsh_A=marsh_a','SmallLakeA=lake_a')
     tex=@() ; texpkg='TextureLibrary_ClimateZones'
     tex2=@('WLD_DirtA_DIF=marsh_dif','WLD_DirtA_NRM=marsh_nrm') }

  # Flat ground grids. These carry no material in the original (each map blends
  # several terrain libraries across them); pointed at wildland dirt here so
  # they load textured. Swap in grass/leaves from the sibling folders.
  @{ id='groundplane'; pkg='GroundPlane'; out='terrain/groundplane'; size=$TerrainSize
     meshes=@('GroundPlane08x08A=groundplane_8x8','GroundPlane08x16A=groundplane_8x16',
              'GroundPlaneAOpen=groundplane_open','TWN_MdSkirt_A=groundplane_skirt_md')
     tex=@() ; texpkg='TextureLibrary_ClimateZones'
     tex2=@('WLD_DirtA_DIF=ground_dif','WLD_DirtA_NRM=ground_nrm') }

  # --- tiling natural surfaces, no meshes ---
  # 'WLD' is XCOM's wildland/temperate climate prefix; ARD is arid, TND tundra.
  @{ id='dirt'; pkg='TextureLibrary_ClimateZones'; out='terrain/dirt'; size=$TerrainSize
     meshes=@()
     tex=@('WLD_DirtA_DIF=dirt_a_dif','WLD_DirtA_NRM=dirt_a_nrm',
           'WLD_DirtB_DIF=dirt_b_dif','WLD_DirtB_NRM=dirt_b_nrm',
           'WLD_DirtC_DIF=dirt_c_dif','WLD_DirtC_NRM=dirt_c_nrm',
           'WLD_GroundA_MSK=ground_msk') }

  @{ id='dirtroad'; pkg='TextureLibrary_Dirt'; out='terrain/dirt'; size=$TerrainSize
     meshes=@()
     tex=@('DirtRoadMossA_DIF=dirtroad_moss_dif','DirtRoadMossA_NRM=dirtroad_moss_nrm',
           'DirtRoadStonesA_DIF=dirtroad_stones_dif','DirtRoadStonesA_NRM=dirtroad_stones_nrm') }

  @{ id='grass'; pkg='TextureLibrary_ClimateZones'; out='terrain/grass'; size=$TerrainSize
     meshes=@()
     tex=@('WLD_GrassA_DIF=grass_a_dif','WLD_GrassA_NRM=grass_a_nrm','WLD_GrassB_DIF=grass_b_dif') }

  @{ id='grasslib'; pkg='TextureLibrary_Grass'; out='terrain/grass'; size=$TerrainSize
     meshes=@()
     tex=@('GrassC_DIF=grass_c_dif','GrassC_NRM=grass_c_nrm','GrassD_DIF=grass_d_dif') }

  @{ id='leaves'; pkg='TextureLibrary_Leaves'; out='terrain/forest_floor'; size=$TerrainSize
     meshes=@()
     tex=@('LeavesA_DIF=leaves_dif','LeavesA_NRM=leaves_nrm') }

  @{ id='roots'; pkg='TextureLibrary_Roots'; out='terrain/forest_floor'; size=$TerrainSize
     meshes=@()
     tex=@('RootsA_DIF=roots_dif','RootsA_NRM=roots_nrm') }

  @{ id='mud'; pkg='TextureLibrary_Mud'; out='terrain/forest_floor'; size=$TerrainSize
     meshes=@()
     tex=@('MudA_DIF=mud_dif','MudA_NRM=mud_nrm',
           'MudTracks_DIF=mud_tracks_dif','MudTracks_NRM=mud_tracks_nrm') }

  @{ id='rocksurface'; pkg='TextureLibrary_Rocks'; out='terrain/forest_floor'; size=$TerrainSize
     meshes=@()
     tex=@('RocksA_DIF=rocks_dif','RocksA_NRM=rocks_nrm') }
)

if ($Only) {
    # Invoked via `powershell -File`, `-Only a,b` arrives as one joined string
    # rather than an array, so split defensively.
    $Only = $Only -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ }
    $kit = $kit | Where-Object { $Only -contains $_.id }
}
if (-not $kit) { throw "No kit entries matched -Only $($Only -join ',')" }

# ------------------------------------------------------------------- extract
if (-not $SkipExtract) {
    $packages = @($kit.pkg) + @($kit | Where-Object { $_.texpkg } | ForEach-Object { $_.texpkg })
    foreach ($p in ($packages | Select-Object -Unique)) {
        & (Join-Path $PSScriptRoot 'xcom_extract.ps1') -Package $p -SdkRoot $SdkRoot |
            Where-Object { $_ -match 'object\(s\)' }
    }
}

# ------------------------------------------------------------------- convert
foreach ($e in $kit) {
    $outDir = Join-Path $repo ("assets\models\" + ($e.out -replace '/', '\'))
    $raw = Join-Path $repo "workbench\xcom_raw\$($e.pkg)"

    $cmd = @('-3', (Join-Path $PSScriptRoot 'xcom_convert.py'),
             '--raw', $raw, '--out', $outDir, '--max-size', $e.size)
    foreach ($m in $e.meshes) { $cmd += @('--mesh', $m) }
    foreach ($t in $e.tex)    { $cmd += @('--tex',  $t) }
    # An empty list means "none", not "all" - say so explicitly, or the
    # converter's explore-the-package default converts the whole package.
    if ($e.meshes.Count -eq 0) { $cmd += '--no-meshes' }
    if (@($e.tex).Count -eq 0) { $cmd += '--no-textures' }

    $dif = @($e.tex) | Where-Object { $_ -match '_dif$' } | Select-Object -First 1
    $nrm = @($e.tex) | Where-Object { $_ -match '_nrm$' } | Select-Object -First 1
    if (-not $dif -and $e.tex2) {
        $dif = @($e.tex2) | Where-Object { $_ -match '_dif$' } | Select-Object -First 1
        $nrm = @($e.tex2) | Where-Object { $_ -match '_nrm$' } | Select-Object -First 1
    }
    if ($e.meshes.Count -gt 0 -and $dif) {
        $cmd += @('--material', $e.id, '--diffuse', (($dif -split '=')[1] + '.png'))
        if ($nrm) { $cmd += @('--normal', (($nrm -split '=')[1] + '.png')) }
    }

    Write-Host "`n--- $($e.id)  [$($e.pkg)] -> assets\models\$($e.out)" -ForegroundColor Cyan
    & py @cmd
    if ($LASTEXITCODE -ne 0) { throw "convert failed for $($e.id)" }

    # Entries whose textures live in a DIFFERENT package need a second pass.
    if ($e.texpkg -and $e.tex2) {
        $cmd2 = @('-3', (Join-Path $PSScriptRoot 'xcom_convert.py'),
                  '--raw', (Join-Path $repo "workbench\xcom_raw\$($e.texpkg)"),
                  '--out', $outDir, '--max-size', $e.size, '--no-meshes')
        foreach ($t in $e.tex2) { $cmd2 += @('--tex', $t) }
        & py @cmd2
        if ($LASTEXITCODE -ne 0) { throw "texture convert failed for $($e.id)" }
    }
}

Write-Host "`nKit rebuilt." -ForegroundColor Green
Get-ChildItem (Join-Path $repo 'assets\models') -Directory | ForEach-Object {
    $cat = $_
    Get-ChildItem $cat.FullName -Directory | ForEach-Object {
        $f = Get-ChildItem $_.FullName -File
        "  {0,-34} {1,2} obj {2,2} png {3,7:N1} MB" -f "$($cat.Name)/$($_.Name)",
            @($f | Where-Object Extension -eq '.obj').Count,
            @($f | Where-Object Extension -eq '.png').Count,
            (($f | Measure-Object Length -Sum).Sum / 1MB)
    }
}
