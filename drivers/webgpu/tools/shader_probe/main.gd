extends Node3D
## Builds a scene that touches as many Forward+ features as possible so that the engine compiles its built-in shaders.


func _material(configure: Callable) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	configure.call(material)
	return material


func _mesh(mesh: Mesh, position: Vector3, material: Material) -> MeshInstance3D:
	var instance := MeshInstance3D.new()
	instance.mesh = mesh
	instance.position = position
	instance.material_override = material
	add_child(instance)
	return instance


func _ready() -> void:
	# Environment: sky, tonemap and every screen-space effect.
	var environment := Environment.new()
	environment.background_mode = Environment.BG_SKY
	var sky := Sky.new()
	sky.sky_material = ProceduralSkyMaterial.new()
	environment.sky = sky
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	environment.reflected_light_source = Environment.REFLECTION_SOURCE_SKY
	environment.tonemap_mode = Environment.TONE_MAPPER_ACES
	environment.ssao_enabled = true
	environment.ssil_enabled = true
	environment.ssr_enabled = true
	environment.sdfgi_enabled = true
	environment.glow_enabled = true
	environment.fog_enabled = true
	environment.volumetric_fog_enabled = true
	environment.adjustment_enabled = true
	environment.adjustment_contrast = 1.1
	var attributes := CameraAttributesPractical.new()
	attributes.dof_blur_far_enabled = true
	attributes.dof_blur_near_enabled = true
	attributes.auto_exposure_enabled = true
	var world := WorldEnvironment.new()
	world.environment = environment
	world.camera_attributes = attributes
	add_child(world)

	var camera := Camera3D.new()
	camera.position = Vector3(0, 2.5, 7)
	camera.rotation_degrees = Vector3(-15, 0, 0)
	add_child(camera)
	camera.make_current()

	# Lights with shadows.
	var sun := DirectionalLight3D.new()
	sun.shadow_enabled = true
	sun.directional_shadow_mode = DirectionalLight3D.SHADOW_PARALLEL_4_SPLITS
	sun.rotation_degrees = Vector3(-50, 30, 0)
	add_child(sun)
	var omni := OmniLight3D.new()
	omni.shadow_enabled = true
	omni.position = Vector3(-2, 2, 1)
	add_child(omni)
	var spot := SpotLight3D.new()
	spot.shadow_enabled = true
	spot.position = Vector3(2, 3, 2)
	spot.rotation_degrees = Vector3(-60, 0, 0)
	add_child(spot)

	# Geometry with materials that enable different shader features.
	_mesh(PlaneMesh.new(), Vector3(0, -0.5, 0), _material.call(func(m): m.albedo_color = Color(0.6, 0.6, 0.6)))
	(get_child(get_child_count() - 1) as MeshInstance3D).scale = Vector3(10, 1, 10)
	_mesh(BoxMesh.new(), Vector3(-3, 0, 0), _material.call(func(m): m.metallic = 0.9; m.roughness = 0.2))
	_mesh(SphereMesh.new(), Vector3(-1, 0, 0), _material.call(func(m): m.emission_enabled = true; m.emission = Color.ORANGE; m.rim_enabled = true))
	_mesh(SphereMesh.new(), Vector3(1, 0, 0), _material.call(func(m): m.clearcoat_enabled = true; m.anisotropy_enabled = true; m.albedo_color = Color.RED))
	_mesh(SphereMesh.new(), Vector3(3, 0, 0), _material.call(func(m): m.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA; m.albedo_color = Color(0.3, 0.6, 1.0, 0.5)))
	_mesh(CapsuleMesh.new(), Vector3(0, 0, -2), _material.call(func(m): m.subsurf_scatter_enabled = true; m.backlight_enabled = true; m.backlight = Color.WHITE; m.refraction_enabled = true))
	_mesh(CylinderMesh.new(), Vector3(2, 0, -2), _material.call(func(m): m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED; m.vertex_color_use_as_albedo = true))
	_mesh(TorusMesh.new(), Vector3(-2, 0, -2), _material.call(func(m): m.cull_mode = BaseMaterial3D.CULL_DISABLED; m.proximity_fade_enabled = true; m.distance_fade_mode = BaseMaterial3D.DISTANCE_FADE_PIXEL_ALPHA))

	var custom := ShaderMaterial.new()
	var shader := Shader.new()
	shader.code = "shader_type spatial; uniform vec4 tint : source_color = vec4(0.2, 1.0, 0.4, 1.0); void fragment() { ALBEDO = tint.rgb; ROUGHNESS = 0.3; }"
	custom.shader = shader
	_mesh(PrismMesh.new(), Vector3(4, 0, -2), custom)

	# Particles, decals, probes and fog volumes.
	var particles := GPUParticles3D.new()
	particles.amount = 64
	particles.process_material = ParticleProcessMaterial.new()
	particles.draw_pass_1 = SphereMesh.new()
	particles.position = Vector3(0, 1, 2)
	add_child(particles)
	var decal := Decal.new()
	decal.size = Vector3(2, 2, 2)
	decal.position = Vector3(0, 0, 2)
	add_child(decal)
	var probe := ReflectionProbe.new()
	probe.size = Vector3(10, 4, 10)
	add_child(probe)
	var fog := FogVolume.new()
	fog.size = Vector3(3, 3, 3)
	fog.material = FogMaterial.new()
	add_child(fog)
	var label := Label3D.new()
	label.text = "shader probe"
	label.position = Vector3(0, 2, 0)
	add_child(label)
	var sprite := Sprite3D.new()
	sprite.texture = PlaceholderTexture2D.new()
	sprite.position = Vector3(-4, 1, 1)
	add_child(sprite)

	# 2D: lights, particles and shapes.
	var layer := CanvasLayer.new()
	add_child(layer)
	var rect := ColorRect.new()
	rect.color = Color(1, 0.5, 0, 0.5)
	rect.size = Vector2(120, 80)
	rect.position = Vector2(20, 20)
	layer.add_child(rect)
	var text := Label.new()
	text.text = "2D label"
	text.position = Vector2(30, 120)
	layer.add_child(text)
	var line := Line2D.new()
	line.points = PackedVector2Array([Vector2(200, 30), Vector2(260, 90), Vector2(320, 30)])
	line.width = 8
	layer.add_child(line)
	var light2d := PointLight2D.new()
	light2d.texture = GradientTexture2D.new()
	light2d.position = Vector2(300, 100)
	light2d.shadow_enabled = true
	layer.add_child(light2d)
	var particles2d := GPUParticles2D.new()
	particles2d.process_material = ParticleProcessMaterial.new()
	particles2d.position = Vector2(400, 100)
	layer.add_child(particles2d)

	# Let the renderer run for a while, then leave.
	await get_tree().create_timer(4.0).timeout
	get_tree().quit()
