<?php

require_once backdrop_get_path('module', 'ai_search') . '/includes/AiSearchBackendPluginBase.inc';


/**
 * Pinecone vector client class.
 */
class AiSearchPineconeVectorClient extends AiSearchVectorClientBase {

  const DEFAULT_TOP_K = 5;

  /**
   * @var array
   */
  protected $options = [];

  /**
   * @param array $options
   *   Options from the Search API server ($this->options).
   */
  public function __construct(array $options = []) {
    $this->options = $options;
  }

  /**
   * Resolve an option from $this->options (not global config).
   *
   * @param string $key
   * @param string $description
   * @param bool $resolve_key_name_with_key_module
   *
   * @return string
   * @throws \Exception
   */
  protected function resolveOption(string $key, string $description, bool $resolve_key_name_with_key_module = FALSE): string {
    $value = $this->options[$key] ?? NULL;

    // If it's a Key name, resolve to secret value.
    if ($resolve_key_name_with_key_module && $value) {
      $resolved = key_get_key_value($value);
      if (!is_string($resolved) || $resolved === '') {
        watchdog('ai_search_pinecone', "Invalid $description (empty resolved value) for option @key.", ['@key' => $key], WATCHDOG_ERROR);
        throw new \Exception("Invalid or missing $description.");
      }
      $value = $resolved;
    }

    if (!is_string($value) || trim($value) === '') {
      watchdog('ai_search_pinecone', "Missing $description for option @key.", ['@key' => $key], WATCHDOG_ERROR);
      throw new \Exception("Invalid or missing $description.");
    }

    return trim($value);
  }

  /**
   * Make a POST request to Pinecone.
   *
   * @param string $path
   *   The API path (e.g. '/query').
   * @param array $payload
   *   JSON payload to send.
   *
   * @return array
   *   Decoded response data.
   *
   * @throws \Exception
   */
  protected function pineconePost(string $path, array $payload = []): array {
    $api_key = $this->resolveOption('pinecone_api_key', 'Pinecone API key', TRUE);
    $host    = $this->resolveOption('pinecone_hostname', 'Pinecone hostname/base URI', TRUE);

    $host = rtrim($host, '/');
    if (strpos($host, 'http://') !== 0 && strpos($host, 'https://') !== 0) {
      $host = 'https://' . $host;
    }
    if (!filter_var($host, FILTER_VALIDATE_URL)) {
      throw new \Exception("Invalid Pinecone base URI: $host");
    }

    $url = $host . $path;
    $response = backdrop_http_request($url, [
      'method'  => 'POST',
      'headers' => [
        'Api-Key'      => $api_key,
        'Content-Type' => 'application/json',
        'Accept'       => 'application/json',
      ],
      'data'    => empty($payload) ? '{}' : json_encode($payload),
      'timeout' => 30,
    ]);

    if (!empty($response->error) && empty($response->data)) {
      throw new \Exception('Pinecone request failed: ' . $response->error);
    }
    $data = json_decode($response->data ?? '', TRUE);
    return is_array($data) ? $data : [];
  }

  /**
   * Helper: safe Top-K.
   */
  protected function resolveTopK($params): int {
    $from_server = isset($this->options['top_k']) ? (int) $this->options['top_k'] : self::DEFAULT_TOP_K;
    $from_call   = isset($params['top_k']) ? (int) $params['top_k'] : NULL;
    $k = $from_call ?: $from_server;
    return max(1, $k);
  }

  /**
   * Log payload to watchdog in DEBUG.
   */
  protected function logPayload(string $action, array $payload): void {
    watchdog('ai_search_pinecone', "Pinecone $action payload: @summary", [
      '@summary' => $this->summarizeLogData($payload),
    ], WATCHDOG_DEBUG);
  }

  /**
   * Log response to watchdog in DEBUG.
   */
  protected function logResponse(string $action, $response): void {
    watchdog('ai_search_pinecone', "Pinecone $action response: @summary", [
      '@summary' => $this->summarizeLogData($response),
    ], WATCHDOG_DEBUG);
  }

  /**
   * Summarize debug data without logging raw vectors or provider responses.
   */
  protected function summarizeLogData($data): string {
    if (is_array($data)) {
      $keys = array_slice(array_keys($data), 0, 8);
      return 'array keys=' . implode(',', $keys) . '; count=' . count($data);
    }
    if (is_object($data)) {
      return 'object class=' . get_class($data);
    }
    if (is_string($data)) {
      return 'string length=' . strlen($data);
    }
    return gettype($data);
  }

  /**
   * Error handler.
   */
  protected function handleError(string $action, \Exception $e): void {
    watchdog('ai_search_pinecone', "Error during Pinecone $action: @message", [
      '@message' => $e->getMessage(),
    ], WATCHDOG_ERROR);
    throw $e;
  }

  /**
   * Query vectors.
   *
   * Expected $parameters:
   *  - vector (array<float>) REQUIRED
   *  - top_k (int) OPTIONAL
   *  - namespace (string) OPTIONAL
   *  - include_metadata (bool) OPTIONAL (defaults true)
   *  - expected_dimension (int) OPTIONAL
   */
  public function query(array $parameters) {
    $expected_dimension = isset($parameters['expected_dimension']) ? (int)$parameters['expected_dimension'] : 1536;
    $vector = $parameters['vector'];
    if (!is_array($vector) || count($vector) !== $expected_dimension) {
      watchdog('ai_search_pinecone', 'Query vector dimension mismatch: expected @exp, got @got', [
        '@exp' => $expected_dimension,
        '@got' => is_array($vector) ? count($vector) : 'N/A',
      ], WATCHDOG_ERROR);
      throw new \Exception('Query vector dimension mismatch.');
    }
    $payload = [
      'vector'          => $vector,
      'topK'            => $this->resolveTopK($parameters),
      'includeMetadata' => array_key_exists('include_metadata', $parameters) ? (bool) $parameters['include_metadata'] : TRUE,
    ];
    if (!empty($parameters['namespace'])) {
      $payload['namespace'] = $parameters['namespace'];
    }
    $this->logPayload('query', $payload);
    try {
      $data = $this->pineconePost('/query', $payload);
      $this->logResponse('query', $data);
      return $data;
    }
    catch (\Exception $e) {
      $this->handleError('query', $e);
    }
  }

  /**
   * Upsert vectors.
   *
   * Expected $parameters:
   *  - vectors (array) REQUIRED
   *  - namespace (string) OPTIONAL
   *  - expected_dimension (int) OPTIONAL
   */
  public function upsert(array $parameters): ?array {
    if (empty($parameters['vectors'])) {
      throw new \Exception('Vectors are required for Pinecone upsert.');
    }
    $expected_dimension = isset($parameters['expected_dimension']) ? (int)$parameters['expected_dimension'] : 1536;
    $filtered_vectors = [];
    foreach ($parameters['vectors'] as $vector) {
      // Only upsert vectors for fields marked as llm_vector_embedding if present.
      if (isset($vector['llm_vector_embedding']) && !$vector['llm_vector_embedding']) {
        continue;
      }
      if (is_array($vector['values']) && count($vector['values']) === $expected_dimension) {
        $filtered_vectors[] = $vector;
      } else {
        watchdog('ai_search_pinecone', 'Upsert vector dimension mismatch: expected @exp, got @got', [
          '@exp' => $expected_dimension,
          '@got' => is_array($vector['values']) ? count($vector['values']) : 'N/A',
        ], WATCHDOG_WARNING);
      }
    }
    if (empty($filtered_vectors)) {
      throw new \Exception('No valid vectors to upsert after dimension validation.');
    }
    $payload = [
      'vectors' => $filtered_vectors,
    ];
    if (!empty($parameters['namespace'])) {
      $payload['namespace'] = $parameters['namespace'];
    }
    $this->logPayload('upsert', $payload);
    try {
      $data = $this->pineconePost('/vectors/upsert', $payload);
      $this->logResponse('upsert', $data);
      return $data;
    }
    catch (\Exception $e) {
      $this->handleError('upsert', $e);
      return NULL;
    }
  }

  /**
   * Describe index stats (namespaces, counts).
   *
   * @return array of rows for display
   */
  public function stats(): array {
    try {
      $stats = $this->pineconePost('/describe_index_stats');
      $this->logResponse('stats', $stats);

      $rows = [];
      if (!empty($stats['namespaces']) && is_array($stats['namespaces'])) {
        foreach ($stats['namespaces'] as $namespace => $info) {
          $rows[] = [
            'Namespace'    => $namespace !== '' ? $namespace : t('No namespace'),
            'Vector Count' => $info['vectorCount'] ?? 0,
          ];
        }
      }
      return $rows;
    }
    catch (\Exception $e) {
      $this->handleError('stats', $e);
      return [];
    }
  }

  /**
   * Delete vectors by IDs or filter.
   *
   * Expected $parameters:
   *  - ids (array<string>) OPTIONAL
   *  - filter (array) OPTIONAL
   *  - namespace (string) OPTIONAL
   */
  public function delete(array $parameters): ?array {
    if (empty($parameters['ids']) && empty($parameters['filter']) && empty($parameters['deleteAll'])) {
      throw new \Exception('Provide "ids", "filter", or "deleteAll" for Pinecone delete().');
    }

    $payload = [];

    if (!empty($parameters['ids'])) {
      $payload['ids'] = array_values($parameters['ids']);
    }
    if (!empty($parameters['filter'])) {
      $payload['filter'] = $parameters['filter'];
    }
    if (!empty($parameters['namespace'])) {
      $payload['namespace'] = $parameters['namespace'];
    }
    if (!empty($parameters['deleteAll'])) {
      $payload['deleteAll'] = TRUE;
    }

    $this->logPayload('delete', $payload);

    try {
      return $this->pineconePost('/vectors/delete', $payload);
    }
    catch (\Exception $e) {
      $this->handleError('delete', $e);
      return NULL;
    }
  }

  /**
   * Delete ALL vectors in a namespace.
   *
   * Expected $parameters:
   *  - namespace (string) REQUIRED
   */
  public function deleteAll(array $parameters): void {
    if (empty($parameters['namespace'])) {
      throw new \Exception('A "namespace" is required for deleteAll().');
    }

    $payload = [
      'deleteAll' => TRUE,
      'namespace' => $parameters['namespace'],
    ];

    $this->logPayload('deleteAll', $payload);

    try {
      $this->pineconePost('/vectors/delete', $payload);
    }
    catch (\Exception $e) {
      $this->handleError('deleteAll', $e);
    }
  }
}
