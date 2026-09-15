import { ref } from "vue";
import type { Candidate, JsonObject, Location } from "./types";

export function createRoutePreview(
  fetchRoute: (
    input: { origin: Location; stationId: string },
    signal: AbortSignal,
  ) => Promise<JsonObject>,
) {
  const candidate = ref<Candidate>();
  const origin = ref<Location>();
  const route = ref<JsonObject>();
  const loading = ref(false);
  const error = ref("");
  let generation = 0;
  let activeController: AbortController | undefined;

  function close() {
    ++generation;
    activeController?.abort();
    activeController = undefined;
    candidate.value = undefined;
    origin.value = undefined;
    route.value = undefined;
    loading.value = false;
    error.value = "";
  }

  async function open(nextCandidate: Candidate, nextOrigin: Location): Promise<boolean> {
    const currentGeneration = ++generation;
    activeController?.abort();
    const controller = new AbortController();
    activeController = controller;
    const capturedOrigin = { ...nextOrigin };
    candidate.value = nextCandidate;
    origin.value = capturedOrigin;
    route.value = undefined;
    loading.value = true;
    error.value = "";
    const isCurrent = () =>
      generation === currentGeneration && !controller.signal.aborted;

    try {
      const result = await fetchRoute(
        { origin: { ...capturedOrigin }, stationId: nextCandidate.stationId },
        controller.signal,
      );
      // Cancellation can race with a response that has already been delivered.
      if (!isCurrent()) return false;
      route.value = result;
      return true;
    } catch (cause) {
      if (isCurrent()) {
        error.value = cause instanceof Error ? cause.message : "路线预览失败，请重试。";
      }
      return false;
    } finally {
      if (isCurrent()) {
        loading.value = false;
        activeController = undefined;
      }
    }
  }

  return { candidate, origin, route, loading, error, open, close };
}
