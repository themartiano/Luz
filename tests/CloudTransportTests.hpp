// Included after the common test helpers in Tests.cpp.
#include <thread>

namespace
{
	void testCloudAccelerationAndReferenceTransport()
	{
		for (CloudType type : {CloudType::Cumulus, CloudType::Stratocumulus,
			CloudType::Stratus, CloudType::Cirrus, CloudType::Cumulonimbus})
		for (unsigned int seed : {17u, 1937u})
		{
			CloudParameters p = CloudVolume::preset(type);
			p.size = Vector3(40, 24, 32);
			p.position = Vector3(0, 6000000, 0);
			p.featureScale = 6;
			p.coverage = 0.75;
			p.seed = seed;
			p.offset = seed == 17u ? Vector3(-5, 1, 3) : Vector3(2, -4, -1);
			p.localMajorants = false; // Evaluate the original field independently of bound-based culling.
			p.weatherVariation = 0.7;
			p.baseVariation = 0.5;
			CloudVolume cloud(p);
			std::size_t nonempty = 0;
			for (int z = 0; z <= 20; z++)
			for (int y = 0; y <= 20; y++)
			for (int x = 0; x <= 20; x++)
			{
				const Vector3 point = p.position + Vector3((x / 20.0 - 0.5) * 40,
					(y / 20.0 - 0.5) * 24, (z / 20.0 - 0.5) * 32);
				const double density = cloud.densityAt(point);
				const double majorant = cloud.densityMajorantAt(point);
				require(density <= majorant + 1e-10, "Cloud local majorant underestimated continuous density.");
				require(majorant >= 0.0 && majorant <= 1.0, "Cloud majorant outside unit interval.");
				nonempty += density > 0.0;
			}
			require(nonempty > 0, "Cloud bound test did not exercise occupied density.");
		}

		CloudParameters p;
		p.size = Vector3(40, 24, 32);
		p.featureScale = 6;
		p.coverage = 0.85;
		p.extinction = 0.12;
		p.maxTrackingSteps = 4096;
		p.multipleScatteringFalloff = 0.4;
		p.directionalCacheResolution = 8;
		CloudVolume cloud(p);
		const Vector3 direction = Utilities::normalize(Vector3(0.2, 1, -0.3));
		double cached[4] = {};
		bool valid[4] = {};
		std::vector<std::thread> workers;
		for (int i = 0; i < 4; i++)
			workers.emplace_back([&, i]() { valid[i] = cloud.directionalOpticalDepth(Vector3(0, -5, 0), direction, cached[i]); });
		for (auto& worker : workers) worker.join();
		for (int i = 0; i < 4; i++)
		{
			require(valid[i], "Concurrent cloud cache lookup failed.");
			requireNear(cached[i], cached[0], "Concurrent cloud cache publication changed result");
		}
		double entry, exit;
		Ray ray = Ray::fromNormalizedDirection(Vector3(0, -5, 0), direction);
		require(cloud.integrationInterval(ray, 0, 1000, entry, exit), "Cloud shadow test missed bounds.");
		constexpr int integrationSteps = 16000;
		const double step = (exit - entry) / integrationSteps;
		double tau = 0;
		for (int i = 0; i < integrationSteps; i++)
			tau += cloud.extinctionAt(ray.pointAtRay(entry + (i + 0.5) * step)) * step;
		const double expected = std::exp(-tau);
		require(expected < 0.98 && expected > 0.01, "Cloud shadow test needs nontrivial optical depth.");
		require(std::fabs(std::exp(-cached[0]) - expected) < 0.035, "Cloud directional cache exceeded transmittance error budget.");
		// Both trackers must converge to numerical transmittance even at later
		// bounces with depth-falloff configured on the cloud.
		double ratioMean = 0, misses = 0;
		constexpr int samples = 12000;
		for (int i = 0; i < samples; i++)
		{
			Sampler::beginPixelSample(57, 19, i);
			Sampler::setBounce(3);
			Sampler::setReferenceVolumeTransport(true);
			Sampler::setDirectionalShadowSampling(true);
			ratioMean += cloud.shadowTransmittance(ray, 0, 1000).getRed();
			misses += !cloud.hitAny(ray, 0, 1000);
			Sampler::endPixelSample();
		}
		require(std::fabs(ratioMean / samples - expected) < 0.025, "Reference cloud ratio tracking is biased.");
		require(std::fabs(misses / samples - expected) < 0.025, "Cloud local-majorant free flight is biased.");
		Sampler::beginPixelSample(0, 0, 0);
		require(!Sampler::isReferenceVolumeTransport(), "Reference mode leaked between samples.");
		Sampler::setVolumeControlSampling(true);
		const double shortShadow = cloud.shadowTransmittance(ray, 0, 0.01).getRed();
		Sampler::endPixelSample();
		require(shortShadow > 0.99, "Truncated cloud shadow incorrectly used full directional cache.");
		double outside;
		require(cloud.directionalOpticalDepth(Vector3(100, 0, 0), Vector3(1, 0, 0), outside)
			&& outside == 0.0, "Cloud cache attenuated a ray facing away from the volume.");
	}

	void testCloudQualityControls()
	{
		const auto path = std::filesystem::temp_directory_path() / "luz_cloud_quality_controls.luz";
		std::vector<std::shared_ptr<CloudVolume>> clouds;
		for (const char* quality : {"preview", "production", "cinematic"})
		{
			{
				std::ofstream file(path);
				file << "[settings]\nvolume_primary_samples=12\nvolume_primary_max_steps=8192\nvolume_reference=1\n\n"
					<< "[scene]\ncloud test {\ntype=cumulus\nquality=" << quality
					<< "\ndirectional_cache_resolution=3\nprimary_detail=3\nweather_variation=0.6\nbase_variation=0.4\ntracking_majorants=0\n}\n";
			}
			Scene scene;
			SceneFile::read(scene, path.string());
			require(scene.getVolumePrimarySamples() == 12 && scene.getVolumePrimaryMaxSteps() == 8192
				&& scene.getVolumeReference(), "Volume integration settings were not parsed.");
			auto cloud = std::dynamic_pointer_cast<CloudVolume>(scene.getHittables().at(0));
			require(cloud != nullptr, "Quality test did not create a cloud.");
			requireNear(cloud->getParameters().primaryDetail, 3.0, "Explicit primary detail precedence");
			requireNear(cloud->getParameters().directionalCacheResolution, 3.0, "Cloud cache resolution parsing");
			require(!cloud->getParameters().localMajorants, "Cloud tracking switch was not parsed.");
			clouds.push_back(cloud);
		}
		std::filesystem::remove(path);
		for (int i = 0; i < 300; i++)
		{
			const Vector3 point((i % 11 - 5) * 250, (i % 13 - 6) * 100, (i % 17 - 8) * 150);
			const double density = clouds[0]->densityAt(point);
			requireNear(clouds[1]->densityAt(point), density, "Production quality changed density");
			requireNear(clouds[2]->densityAt(point), density, "Cinematic quality changed density");
		}
		// Reference mode must suppress the deterministic primary contribution,
		// even when the sample caller asks to calculate it.
		Scene referenceScene;
		referenceScene.setRenderSky(SKY_NONE);
		referenceScene.setBackgroundColor(Color(0, 0, 0));
		referenceScene.setMaxLightBounces(0);
		referenceScene.addHittable(std::make_shared<TestPrimaryDensityVolume>(2, 6, 0.25));
		referenceScene.addHittable(std::make_shared<DirectionalLight>(Vector3(-1, 0, 0),
			std::make_shared<Emissive>(Color(1, 1, 1))));
		referenceScene.updateLights();
		Renderer::internal::RenderCamera camera;
		camera.width = camera.height = camera.inverseWidth = camera.inverseHeight = 1;
		camera.position = Vector3(0, 0, 0);
		camera.lowerLeftCorner = Vector3(0, 0, -1);
		for (bool reference : {false, true})
		{
			referenceScene.setVolumeReference(reference);
			Sampler::beginPixelSample(29, 31, 0);
			const auto sample = Renderer::internal::_calculatePixelSample(referenceScene, camera, 0, 0, true);
			Sampler::endPixelSample();
			const double energy = Utilities::luminance(sample.primarySingleScattering);
			require(reference ? energy == 0.0 : energy > 0.0, "Reference renderer did not bypass primary cloud lighting.");
		}
		Scene scene;
		requireThrows([&]() { scene.setVolumePrimarySamples(0); }, "Zero primary samples accepted.");
		requireThrows([&]() { scene.setVolumePrimaryMaxSteps(1000000); }, "Unbounded primary march accepted.");
		CloudParameters p;
		p.directionalCacheResolution = -1;
		requireThrows([&]() { CloudVolume invalid(p); }, "Negative cache resolution accepted.");
	}
}
