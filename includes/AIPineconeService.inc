<?php

/**
 * Search service class for Pinecone in Backdrop CMS.
 */

require_once backdrop_get_path('module', 'ai_search') . '/includes/AiSearchBackendPluginBase.inc';
require_once backdrop_get_path('module', 'ai_search') . '/includes/AiSearchBackendInterface.inc';
require_once backdrop_get_path('module', 'ai_search') . '/includes/AiSearchBackend.inc';

class AIPineconeService extends SearchApiAbstractService {

  /**
   * @var AiSearchPineconeVectorClient
   */
  protected $client;

  /**
   * {@inheritdoc}
   */
  public function __construct(SearchApiServer $server) {
    parent::__construct($server);
    require_once __DIR__ . '/AiSearchPineconeVectorClient.php';
    // Client reads $this->options (server settings) incl. key names.
    $this->client = new AiSearchPineconeVectorClient($this->options);
  }

  /**
   * Server configuration form (saved by the Search API backend layer).
   */
  public function configurationForm($form, &$form_state) {
    $embedding_configuration = ai_search_default_engine_configuration();
    if (!empty($this->options['embeddings_engine'])) {
      $embedding_configuration['embeddings_engine'] = $this->options['embeddings_engine'];
    }
    if (!empty($this->options['embeddings_engine_configuration'])) {
      $embedding_configuration['embeddings_engine_configuration'] = $this->options['embeddings_engine_configuration'];
    }
    $form = ai_search_engine_configuration_form($form, $form_state, $embedding_configuration);

    // Optional namespace prefix (index ID is always appended).
    $form['namespace'] = [
      '#type' => 'textfield',
      '#title' => t('Namespace'),
      '#description' => t('Optional prefix. The index ID will always be appended.'),
      '#default_value' => !empty($this->options['namespace']) ? $this->options['namespace'] : '',
      '#pattern' => '[a-zA-Z0-9_\-:]*',
    ];

    // Pinecone API key stored via Key module.
    $form['pinecone_api_key'] = [
      '#type' => 'key_select',
      '#title' => t('Pinecone API Key'),
      '#default_value' => !empty($this->options['pinecone_api_key']) ? $this->options['pinecone_api_key'] : '',
      '#options' => function_exists('key_get_key_names_as_options') ? key_get_key_names_as_options() : [],
      '#key_filters' => ['type' => 'authentication'],
      '#required' => TRUE,
      '#description' => t('Select the Key that stores your Pinecone API key.'),
    ];

    // Pinecone hostname/base URL stored via Key module.
    $form['pinecone_hostname'] = [
      '#type' => 'key_select',
      '#title' => t('Pinecone Hostname / Base URL'),
      '#default_value' => !empty($this->options['pinecone_hostname']) ? $this->options['pinecone_hostname'] : '',
      '#options' => function_exists('key_get_key_names_as_options') ? key_get_key_names_as_options() : [],
      '#key_filters' => ['type' => 'authentication'],
      '#required' => TRUE,
      '#description' => t('Select the Key that stores your Pinecone base URL (e.g., <code>https://...pinecone.io</code>).'),
    ];

    // Optional: default Top-K (nice to have; can be omitted).
    $form['top_k'] = [
      '#type' => 'number',
      '#title' => t('Default Top-K'),
      '#default_value' => isset($this->options['top_k']) ? (int) $this->options['top_k'] : 5,
      '#min' => 1,
      '#step' => 1,
      '#description' => t('Default number of results; can be overridden per query.'),
    ];

    $form = ai_search_reranking_configuration_form($form, $form_state, $this->options);
    return $form;
  }

  /**
   * Index items to Pinecone (batch upsert).
   */
  public function indexItems(SearchApiIndex $index, array $items) {
    global $ai_search_is_indexing;
    $ai_search_is_indexing = TRUE;

    // Dimension from server options or global fallback.
    $expected_dimension = isset($this->options['embeddings_engine_configuration']['dimension'])
      ? (int) $this->options['embeddings_engine_configuration']['dimension']
      : ai_search_get_embedding_dimension();

    $namespace = $this->getNamespace($index);
    $successful_item_ids = [];
    $batch_vectors = [];
    $ids_by_type = [];

    if (empty($items)) {
      return [];
    }

    foreach ($items as $item) {
      if (is_array($item)) {
        $item = (object) $item;
      }

      if (isset($item->nid)) {
        $ids_by_type['node'][] = is_array($item->nid) && isset($item->nid['value'])
          ? (int) $item->nid['value']
          : (int) $item->nid;
      }
      elseif (isset($item->tid)) {
        $ids_by_type['taxonomy_term'][] = is_array($item->tid) && isset($item->tid['value'])
          ? (int) $item->tid['value']
          : (int) $item->tid;
      }
      elseif (isset($item->item_id)) {
        $ids_by_type['paragraphs_item'][] = is_array($item->item_id) && isset($item->item_id['value'])
          ? (int) $item->item_id['value']
          : (int) $item->item_id;
      }
    }

    foreach ($ids_by_type as $entity_type => $ids) {
      foreach (array_unique($ids) as $entity_id) {
        $rows = ai_search_normalize_vectors($entity_type, $entity_id, $expected_dimension, $index->machine_name);
        if (empty($rows)) {
          continue;
        }

        foreach ($rows as $r) {
          $pinecone_id = "entity:{$entity_type}/{$entity_id}:en:{$r['field_name']}:{$r['chunk_index']}";

          $batch_vectors[] = [
            'id'       => $pinecone_id,
            'values'   => $r['values'],
            'metadata' => $r['metadata'],
            'llm_vector_embedding' => $r['llm_vector_embedding'],
          ];
        }

        $successful_item_ids[] = $entity_id;
      }
    }

    if (!empty($batch_vectors)) {
      try {
        $this->client->upsert([
          'vectors'   => $batch_vectors,
          'namespace' => $namespace,
          'expected_dimension' => $expected_dimension,
        ]);
        //watchdog('ai_search_pinecone', 'Indexed @count vectors.',
        // ['@count' => count($batch_vectors)], WATCHDOG_NOTICE];
      }
      catch (\Exception $e) {
        watchdog('ai_search_pinecone', 'Upsert error: @m', ['@m' => $e->getMessage()], WATCHDOG_ERROR);
      }
    }

    return $successful_item_ids;
  }

  /**
   * Vector search in Pinecone.
   * Safeguard: Prevent searches during indexing. Logs a warning if attempted.
   * Indexing is NOT interrupted if a search is attempted; only the search is blocked.
   */
  public function search(SearchApiQueryInterface $query) {
    global $ai_search_is_indexing;
    if (!empty($ai_search_is_indexing)) {
      return ['result count' => 0, 'results' => []];
    }

    $index = $query->getIndex();
    $index_options = isset($index->options) ? $index->options : [];

    // Model selection: server options (canonical) → service embedding_model fallback.
    $model = $this->options['embeddings_engine'] ?? $this->options['embedding_model'] ?? NULL;

    if (empty($model)) {
      watchdog(
        'ai_search_pinecone',
        'No model configured!',
        [],
        WATCHDOG_ERROR
      );
      return ['result count' => 0, 'results' => []];
    }

    // Extract search text.
    $keys = $query->getKeys();
    $text = is_array($keys)
      ? implode(' ', array_filter($keys, function($key) {
        return is_string($key) && $key !== 'AND' && $key !== '#conjunction';
      }))
      : (string) $keys;
    $text = trim($text);

    if (empty($text)) {
      return ['result count' => 0, 'results' => []];
    }

    // Get query embedding using the appropriate provider via compatibility helper.
    $stripped_model = NULL;
    $api = ai_get_api_for_model($model, $stripped_model);
    $model_to_use = !empty($stripped_model) ? $stripped_model : $model;

    if ($api && method_exists($api, 'recordLog')) {
      $api->recordLog('search_embedding', $model, ['text_len' => strlen($text)], NULL, TRUE, 0);
    }

    // Final fallback: if helper didn't return a client, try legacy instantiation.
    if (!$api) {
      watchdog('ai_search_pinecone', 'No AI client available for model: @model', ['@model' => $model], WATCHDOG_ERROR);
      return ['result count' => 0, 'results' => []];
    }

    // Request embedding using the compatibility wrapper which understands different client shapes.
    $query_embedding = $api->embedding($text, $model_to_use);

    if (empty($query_embedding)) {
      throw new Exception('Failed to retrieve embedding.');
    }

    // Get expected dimension from server options, fallback to global.
    $expected_dimension = isset($this->options['embeddings_engine_configuration']['dimension'])
      ? (int) $this->options['embeddings_engine_configuration']['dimension']
      : ai_search_get_embedding_dimension();

    if (!is_array($query_embedding) || count($query_embedding) !== $expected_dimension) {
      $actual_dimension = (is_array($query_embedding) && !empty($query_embedding)) ? count($query_embedding) : 0;
      watchdog(
        'ai_search_pinecone',
        'Query embedding dimension mismatch or empty. Expected: @expected_dim, actual: @actual_dim.',
        [
          '@expected_dim' => $expected_dimension,
          '@actual_dim' => $actual_dimension,
        ],
        WATCHDOG_ERROR
      );
      return ['result count' => 0, 'results' => []];
    }

    try {
      $response = $this->client->query([
        'vector' => $query_embedding,
        'top_k' => $query->getOption('top_k') ?? ($this->options['top_k'] ?? 5),
        'include_metadata' => TRUE,
        'namespace' => $this->getNamespace($index),
        'expected_dimension' => $expected_dimension,
      ]);

      $raw_matches = is_array($response) && isset($response['matches']) ? $response['matches'] : [];

      // Normalize for reranking: Pinecone stores metadata in a 'metadata' subkey.
      $normalized_matches = [];
      foreach ($raw_matches as $match) {
        $normalized_matches[] = [
          'score'    => (float) ($match['score'] ?? 0),
          'metadata' => $match['metadata'] ?? [],
          '_raw'     => $match,
        ];
      }

      $rerank_settings = !empty($this->options['metadata_reranking']) ? $this->options['metadata_reranking'] : [];
      ai_search_apply_metadata_reranking($normalized_matches, $text, $rerank_settings);

      $results = [];
      foreach ($normalized_matches as $nm) {
        $match = $nm['_raw'];
        if (preg_match('/^entity:([^\/]+)\/(\d+):/', $match['id'], $m)) {
          $entity_type = $m[1];
          $entity_id = $m[2];

          $item_id = $index->getEntityType() === $entity_type ? $entity_id : "{$entity_type}/{$entity_id}";
          $results[$item_id] = [
            'item_id' => $item_id,
            'score'   => $nm['score'],
            'fields'  => [],
            'extra_data' => [
              'metadata'   => $match['metadata'] ?? [],
              'pinecone_id' => $match['id'],
            ],
          ];
        }
      }

      return ['result count' => count($results), 'results' => $results];
    }
    catch (\Exception $e) {
      watchdog('ai_search_pinecone', 'Query error: @m', ['@m' => $e->getMessage()], WATCHDOG_ERROR);
      return ['result count' => 0, 'results' => []];
    }
  }

  /**
   * Delete vectors.
   */
  public function deleteItems($ids = 'all', SearchApiIndex $index = NULL) {
    global $ai_search_is_indexing;
    if (!empty($ai_search_is_indexing)) {
      return;
    }

    $namespace = $this->getNamespace($index);
    $payload = ['namespace' => $namespace];

    if ($ids === 'all') {
      $payload['deleteAll'] = TRUE;
    } else {
      if (empty($ids) || !is_array($ids)) {
        return;
      }
      $entity_type = $index ? $index->getEntityType() : 'node';
      $entity_ids = array_values(array_unique(array_map('intval', array_filter($ids))));
      if (empty($entity_ids)) {
        return;
      }
      $payload['filter'] = [
        '$and' => [
          ['entity_type' => ['$eq' => $entity_type]],
          ['entity_id' => ['$in' => $entity_ids]],
        ],
      ];
    }

    try {
      $this->client->delete($payload);
    }
    catch (\Exception $e) {
      watchdog('ai_search_pinecone', 'Delete error: @m', ['@m' => $e->getMessage()], WATCHDOG_ERROR);
    }
  }

  /**
   * Extract metadata from a result row.
   */
  public function extractMetadata($result_row, $item) {
    $item->extra_data['metadata'] = $result_row['metadata'];
  }

  public function getNamespace(SearchApiIndex $index) {
    $ns = isset($this->options['namespace']) ? trim((string) $this->options['namespace']) : '';
    if ($ns === '') {
      $ns = $index->machine_name; // e.g. "pinecone_search"
    }
    // Pinecone-friendly chars only
    return preg_replace('/[^A-Za-z0-9_\-]/', '_', $ns);
  }

  /**
   * Supported data types.
   */
  public function supportsDataType($type) {
    return in_array($type, ['embeddings']);
  }
}
