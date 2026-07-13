#!/bin/bash
set -euo pipefail

cd "${BENCH_ROOT:-/luz}"

make luz >&2

repeat=${BENCH_REPEAT:-20}
warmup=${BENCH_WARMUP:-2}
seed=${BENCH_SEED:-424242424}
threads=${BENCH_THREADS:-1}
cases=${BENCH_CASES:-${BENCH_CASE:-default many-objects mesh-bvh diffuse postprocess atmosphere lights emissive-geometry primitives-materials volumes obj-mesh suzanne-single suzanne-instances}}
global_width=${BENCH_WIDTH:-}
global_height=${BENCH_HEIGHT:-}
global_samples=${BENCH_SAMPLES:-}
global_bounces=${BENCH_BOUNCES:-}
global_view_transform=${BENCH_VIEW_TRANSFORM:-}
global_bloom=${BENCH_BLOOM:-}
global_adaptive=${BENCH_ADAPTIVE:-}
global_denoise=${BENCH_DENOISE:-}
global_adaptive_min_samples=${BENCH_ADAPTIVE_MIN_SAMPLES:-}
global_adaptive_threshold=${BENCH_ADAPTIVE_THRESHOLD:-}
global_adaptive_check_interval=${BENCH_ADAPTIVE_CHECK_INTERVAL:-}
score_sample_unit=${BENCH_SCORE_SAMPLE_UNIT:-1000}
git_commit=${BENCH_GIT_COMMIT:-unknown}
git_status=${BENCH_GIT_STATUS:-unknown}
system=$(uname -srm | tr ',' ';')
compiler=$(clang++ --version | head -n 1 | tr ',' ';')

if (( repeat <= 0 )); then
	echo "BENCH_REPEAT must be positive." >&2
	exit 1
fi

if (( warmup < 0 )); then
	echo "BENCH_WARMUP cannot be negative." >&2
	exit 1
fi

awk -v score_sample_unit="$score_sample_unit" '
	BEGIN {
		if (score_sample_unit !~ /^[0-9]+(\.[0-9]+)?$/ || score_sample_unit <= 0) {
			exit 1
		}
	}
' || {
	echo "BENCH_SCORE_SAMPLE_UNIT must be positive." >&2
	exit 1
}

case_defaults()
{
	local benchmark_case=$1
	case "$benchmark_case" in
		default)
			echo "builtin,${global_width:-200},${global_height:-200},${global_samples:-50},${global_bounces:-8},${global_view_transform:-agx},${global_bloom:-true},${global_adaptive:-false},${global_denoise:-false},${global_adaptive_min_samples:-},${global_adaptive_threshold:-},${global_adaptive_check_interval:-}"
			;;
		many-objects)
			echo "builtin,${global_width:-160},${global_height:-160},${global_samples:-16},${global_bounces:-5},${global_view_transform:-agx},${global_bloom:-false},${global_adaptive:-false},${global_denoise:-false},${global_adaptive_min_samples:-},${global_adaptive_threshold:-},${global_adaptive_check_interval:-}"
			;;
		mesh-bvh)
			echo "builtin,${global_width:-160},${global_height:-160},${global_samples:-16},${global_bounces:-5},${global_view_transform:-agx},${global_bloom:-false},${global_adaptive:-false},${global_denoise:-false},${global_adaptive_min_samples:-},${global_adaptive_threshold:-},${global_adaptive_check_interval:-}"
			;;
		diffuse)
			echo "builtin,${global_width:-180},${global_height:-180},${global_samples:-32},${global_bounces:-7},${global_view_transform:-agx},${global_bloom:-false},${global_adaptive:-false},${global_denoise:-false},${global_adaptive_min_samples:-},${global_adaptive_threshold:-},${global_adaptive_check_interval:-}"
			;;
		postprocess)
			echo "builtin,${global_width:-360},${global_height:-360},${global_samples:-2},${global_bounces:-1},${global_view_transform:-agx},${global_bloom:-true},${global_adaptive:-false},${global_denoise:-false},${global_adaptive_min_samples:-},${global_adaptive_threshold:-},${global_adaptive_check_interval:-}"
			;;
		atmosphere)
			echo "builtin,${global_width:-160},${global_height:-160},${global_samples:-6},${global_bounces:-3},${global_view_transform:-agx},${global_bloom:-false},${global_adaptive:-false},${global_denoise:-false},${global_adaptive_min_samples:-},${global_adaptive_threshold:-},${global_adaptive_check_interval:-}"
			;;
		lights)
			echo "builtin,${global_width:-180},${global_height:-180},${global_samples:-24},${global_bounces:-6},${global_view_transform:-agx},${global_bloom:-false},${global_adaptive:-false},${global_denoise:-false},${global_adaptive_min_samples:-},${global_adaptive_threshold:-},${global_adaptive_check_interval:-}"
			;;
		emissive-geometry)
			echo "builtin,${global_width:-180},${global_height:-180},${global_samples:-18},${global_bounces:-5},${global_view_transform:-agx},${global_bloom:-true},${global_adaptive:-false},${global_denoise:-false},${global_adaptive_min_samples:-},${global_adaptive_threshold:-},${global_adaptive_check_interval:-}"
			;;
		primitives-materials)
			echo "builtin,${global_width:-180},${global_height:-180},${global_samples:-24},${global_bounces:-7},${global_view_transform:-agx},${global_bloom:-false},${global_adaptive:-false},${global_denoise:-false},${global_adaptive_min_samples:-},${global_adaptive_threshold:-},${global_adaptive_check_interval:-}"
			;;
		volumes)
			echo "builtin,${global_width:-160},${global_height:-160},${global_samples:-18},${global_bounces:-8},${global_view_transform:-agx},${global_bloom:-false},${global_adaptive:-false},${global_denoise:-false},${global_adaptive_min_samples:-},${global_adaptive_threshold:-},${global_adaptive_check_interval:-}"
			;;
		obj-mesh)
			echo "builtin,${global_width:-160},${global_height:-160},${global_samples:-16},${global_bounces:-6},${global_view_transform:-agx},${global_bloom:-false},${global_adaptive:-false},${global_denoise:-false},${global_adaptive_min_samples:-},${global_adaptive_threshold:-},${global_adaptive_check_interval:-}"
			;;
		suzanne-single)
			echo "examples/scenes/suzanne.luz,${global_width:-120},${global_height:-80},${global_samples:-4},${global_bounces:-4},${global_view_transform:-standard},${global_bloom:-false},${global_adaptive:-false},${global_denoise:-false},${global_adaptive_min_samples:-},${global_adaptive_threshold:-},${global_adaptive_check_interval:-}"
			;;
		suzanne-instances)
			echo "examples/scenes/suzanne-instances.luz,${global_width:-120},${global_height:-80},${global_samples:-2},${global_bounces:-4},${global_view_transform:-standard},${global_bloom:-false},${global_adaptive:-false},${global_denoise:-false},${global_adaptive_min_samples:-},${global_adaptive_threshold:-},${global_adaptive_check_interval:-}"
			;;
		stormtroopers-preview)
			echo "exports/stormtroopers.luz,${global_width:-80},${global_height:-45},${global_samples:-8},${global_bounces:-5},${global_view_transform:-standard},${global_bloom:-true},${global_adaptive:-false},${global_denoise:-false},${global_adaptive_min_samples:-},${global_adaptive_threshold:-},${global_adaptive_check_interval:-}"
			;;
		stormtroopers-adaptive)
			echo "exports/stormtroopers.luz,${global_width:-80},${global_height:-45},${global_samples:-128},${global_bounces:-5},${global_view_transform:-standard},${global_bloom:-true},${global_adaptive:-true},${global_denoise:-false},${global_adaptive_min_samples:-},${global_adaptive_threshold:-},${global_adaptive_check_interval:-}"
			;;
		stormtroopers-adaptive-tuned)
			echo "exports/stormtroopers.luz,${global_width:-80},${global_height:-45},${global_samples:-64},${global_bounces:-5},${global_view_transform:-standard},${global_bloom:-true},${global_adaptive:-true},${global_denoise:-false},${global_adaptive_min_samples:-16},${global_adaptive_threshold:-0.02},${global_adaptive_check_interval:-8}"
			;;
		stormtroopers-denoise-micro)
			echo "exports/stormtroopers.luz,${global_width:-40},${global_height:-23},${global_samples:-32},${global_bounces:-5},${global_view_transform:-standard},${global_bloom:-true},${global_adaptive:-true},${global_denoise:-true},${global_adaptive_min_samples:-16},${global_adaptive_threshold:-0.02},${global_adaptive_check_interval:-8}"
			;;
		stormtroopers-denoise)
			echo "exports/stormtroopers.luz,${global_width:-80},${global_height:-45},${global_samples:-64},${global_bounces:-5},${global_view_transform:-standard},${global_bloom:-true},${global_adaptive:-true},${global_denoise:-true},${global_adaptive_min_samples:-16},${global_adaptive_threshold:-0.02},${global_adaptive_check_interval:-8}"
			;;
		stormtroopers-command)
			echo "exports/stormtroopers.luz,${global_width:-320},${global_height:-180},${global_samples:-128},${global_bounces:-5},${global_view_transform:-standard},${global_bloom:-true},${global_adaptive:-true},${global_denoise:-true},${global_adaptive_min_samples:-},${global_adaptive_threshold:-},${global_adaptive_check_interval:-}"
			;;
		*)
			echo "Unknown benchmark case: $benchmark_case" >&2
			exit 1
			;;
	esac
}

print_case_summary_and_score()
{
	local benchmark_case=$1
	local width=$2
	local height=$3
	local samples=$4
	shift 4
	printf '%s\n' "$@" | sort -n | awk -v benchmark_case="$benchmark_case" \
		-v width="$width" \
		-v height="$height" \
		-v samples="$samples" \
		-v score_sample_unit="$score_sample_unit" '
		{
			values[NR] = $1
			sum += $1
		}
		END {
			if (NR == 0) {
				exit 0
			}
			if (NR % 2 == 1) {
				median = values[int(NR / 2) + 1]
			} else {
				median = (values[NR / 2] + values[NR / 2 + 1]) / 2.0
			}
			mean = sum / NR
			for (i = 1; i <= NR; i++) {
				diff = values[i] - mean
				variance += diff * diff
			}
			stddev = sqrt(variance / NR)
			paths = width * height * samples
			samples_per_second = paths / (median / 1000.0)
			score = samples_per_second * 60.0 / score_sample_unit
			printf "summary case=%s min_ms=%.6f median_ms=%.6f mean_ms=%.6f max_ms=%.6f stddev_ms=%.6f\n", benchmark_case, values[1], median, mean, values[NR], stddev > "/dev/stderr"
			printf "score case=%s median_samples_per_second=%.6f score=%.2f\n", benchmark_case, samples_per_second, score > "/dev/stderr"
			printf "%.12f\n", score
		}
	'
}

print_overall_score()
{
	printf '%s\n' "$@" | awk '
		{
			if ($1 <= 0) {
				exit 1
			}
			sum_log += log($1)
			count++
		}
		END {
			if (count == 0) {
				exit 1
			}
			score = exp(sum_log / count)
			printf "score overall=%.2f\n", score > "/dev/stderr"
		}
	'
}

elapsed_from_output()
{
	awk '
		{
			line = $0
			gsub(/\033\[[0-9;]*[A-Za-z]/, "", line)
		}
		line ~ /^[[:space:]]*[0-9]+([.][0-9]+)?[[:space:]]*$/ {
			value = line
			gsub(/^[[:space:]]+|[[:space:]]+$/, "", value)
		}
		END {
			if (value == "") {
				exit 1
			}
			print value
		}
	'
}

stat_from_output()
{
	local key=$1

	awk -v key="$key" '
		{
			line = $0
			gsub(/\033\[[0-9;]*[A-Za-z]/, "", line)
		}
		line ~ /^stats[[:space:]]/ {
			count = split(line, fields, /[[:space:]]+/)
			for (i = 1; i <= count; i++) {
				split(fields[i], pair, "=")
				if (pair[1] == key) {
					value = pair[2]
				}
			}
		}
		END {
			if (value == "") {
				exit 1
			}
			print value
		}
	'
}

monotonic_ns()
{
	local value

	value=$(date +%s%N 2>/dev/null || true)
	if [[ "$value" =~ ^[0-9]+$ ]]; then
		printf '%s' "$value"
	fi
}

echo "case,run,elapsed_ms,samples_per_second,width,height,samples,bounces,threads,seed,view_transform,bloom,adaptive,denoise,git_commit,git_status,compiler,system,score,rendered_samples,average_spp,actual_samples_per_second,actual_score,render_samples_per_second,render_score,model_load_ms,scene_build_ms,render_ms,denoise_ms,postprocess_ms,total_ms"

case_scores=()
for benchmark_case in $cases; do
	IFS=',' read -r source width height samples bounces view_transform bloom adaptive denoise adaptive_min_samples adaptive_threshold adaptive_check_interval <<< "$(case_defaults "$benchmark_case")"

	if [[ "$source" != "builtin" && ! -f "$source" ]]; then
		echo "Skipping $benchmark_case because scene file is missing: $source" >&2
		continue
	fi

	benchmark_args=(
		--seed "$seed"
		--threads "$threads"
		--resolution "${width}x${height}"
		--samples "$samples"
		--maxLightBounces "$bounces"
		--adaptive "$adaptive"
		--view-transform "$view_transform"
		--bloom "$bloom"
		--denoise "$denoise"
	)

	if [[ -n "$adaptive_min_samples" ]]; then
		benchmark_args+=(--adaptive-min-samples "$adaptive_min_samples")
	fi
	if [[ -n "$adaptive_threshold" ]]; then
		benchmark_args+=(--adaptive-threshold "$adaptive_threshold")
	fi
	if [[ -n "$adaptive_check_interval" ]]; then
		benchmark_args+=(--adaptive-check-interval "$adaptive_check_interval")
	fi
	if [[ "$source" == "builtin" ]]; then
		benchmark_args=(--benchmark --benchmark-case "$benchmark_case" "${benchmark_args[@]}")
	else
		benchmark_args=(--file "$source" --benchmark "${benchmark_args[@]}")
	fi

	echo "benchmark=$benchmark_case source=$source seed=$seed threads=$threads resolution=${width}x${height} samples=$samples bounces=$bounces adaptive=$adaptive denoise=$denoise view_transform=$view_transform bloom=$bloom warmup=$warmup repeat=$repeat" >&2

	for ((n = 0; n < warmup; n++)); do
		./luz "${benchmark_args[@]}" >/dev/null
	done

	times=()
	total_paths=$((width * height * samples))
	for ((n = 1; n <= repeat; n++)); do
		start_ns=$(monotonic_ns)
		run_output=$(./luz "${benchmark_args[@]}")
		end_ns=$(monotonic_ns)
		render_elapsed_ms=$(printf '%s\n' "$run_output" | elapsed_from_output) || {
			printf '%s\n' "$run_output" >&2
			echo "Could not parse elapsed benchmark time for $benchmark_case run $n." >&2
			exit 1
		}
		if [[ -n "$start_ns" && -n "$end_ns" ]]; then
			elapsed_ms=$(awk -v start_ns="$start_ns" -v end_ns="$end_ns" 'BEGIN { printf "%.6f", (end_ns - start_ns) / 1000000.0 }')
		else
			elapsed_ms=$render_elapsed_ms
		fi
		times+=("$elapsed_ms")
		samples_per_second=$(awk -v paths="$total_paths" -v ms="$elapsed_ms" 'BEGIN { printf "%.6f", paths / (ms / 1000.0) }')
		score=$(awk -v samples_per_second="$samples_per_second" -v score_sample_unit="$score_sample_unit" 'BEGIN { printf "%.2f", samples_per_second * 60.0 / score_sample_unit }')
		rendered_samples=$(printf '%s\n' "$run_output" | stat_from_output rendered_samples || true)
		average_spp=$(printf '%s\n' "$run_output" | stat_from_output avg_spp || true)
		model_load_ms=$(printf '%s\n' "$run_output" | stat_from_output model_load_ms || true)
		scene_build_ms=$(printf '%s\n' "$run_output" | stat_from_output scene_build_ms || true)
		render_ms=$(printf '%s\n' "$run_output" | stat_from_output render_ms || true)
		denoise_ms=$(printf '%s\n' "$run_output" | stat_from_output denoise_ms || true)
		postprocess_ms=$(printf '%s\n' "$run_output" | stat_from_output postprocess_ms || true)
		total_ms=$(printf '%s\n' "$run_output" | stat_from_output total_ms || true)
		if [[ -z "$rendered_samples" ]]; then
			rendered_samples=$total_paths
		fi
		if [[ -z "$average_spp" ]]; then
			average_spp=$samples
		fi
		actual_samples_per_second=$(awk -v samples="$rendered_samples" -v ms="$elapsed_ms" 'BEGIN { printf "%.6f", samples / (ms / 1000.0) }')
		actual_score=$(awk -v samples_per_second="$actual_samples_per_second" -v score_sample_unit="$score_sample_unit" 'BEGIN { printf "%.2f", samples_per_second * 60.0 / score_sample_unit }')
		render_samples_per_second=""
		render_score=""
		if [[ -n "$render_ms" ]]; then
			render_samples_per_second=$(awk -v samples="$rendered_samples" -v ms="$render_ms" 'BEGIN { if (ms > 0) printf "%.6f", samples / (ms / 1000.0) }')
			if [[ -n "$render_samples_per_second" ]]; then
				render_score=$(awk -v samples_per_second="$render_samples_per_second" -v score_sample_unit="$score_sample_unit" 'BEGIN { printf "%.2f", samples_per_second * 60.0 / score_sample_unit }')
			fi
		fi
		echo "$benchmark_case,$n,$elapsed_ms,$samples_per_second,$width,$height,$samples,$bounces,$threads,$seed,$view_transform,$bloom,$adaptive,$denoise,$git_commit,$git_status,$compiler,$system,$score,$rendered_samples,$average_spp,$actual_samples_per_second,$actual_score,$render_samples_per_second,$render_score,$model_load_ms,$scene_build_ms,$render_ms,$denoise_ms,$postprocess_ms,$total_ms"
	done

	case_score=$(print_case_summary_and_score "$benchmark_case" "$width" "$height" "$samples" "${times[@]}")
	case_scores+=("$case_score")
done

print_overall_score "${case_scores[@]}"
