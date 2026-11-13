#ifndef PURPLEDB_C_H_
#define PURPLEDB_C_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Return Codes
 * --------------------------------------------------------------------------- */
#define PDB_OK 0
#define PDB_ERROR -1
#define PDB_NOT_FOUND -2
#define PDB_INVALID_ARGUMENT -3

/* ---------------------------------------------------------------------------
 * Opaque Handles
 * --------------------------------------------------------------------------- */
typedef struct pdb_t pdb_t;
typedef struct pdb_results_t pdb_results_t;

/* ---------------------------------------------------------------------------
 * ABI Version
 * --------------------------------------------------------------------------- */
/**
 * @brief Returns the stable PurpleDB C ABI version.
 * Current specification: Version 1.
 */
int pdb_abi_version(void);

/**
 * @brief Returns the thread-local error message for the last failed operation.
 * Returns empty string "" if the last operation succeeded.
 */
const char* pdb_last_error(void);

/* ---------------------------------------------------------------------------
 * Database Configuration Options
 * --------------------------------------------------------------------------- */
typedef struct pdb_options_t {
  int create_if_missing;        /* 1 = create if missing, 0 = fail */
  int error_if_exists;          /* 1 = fail if exists, 0 = open existing */
  size_t write_buffer_size;     /* MemTable flush threshold (bytes), 0 = default (4MB) */
  size_t block_cache_size;      /* Block cache capacity (bytes), 0 = default (8MB) */
  int enable_hnsw;              /* 1 = HNSW proximity graph enabled, 0 = flat scan */
  uint32_t hnsw_M;              /* Max graph links per node (e.g. 16) */
  uint32_t hnsw_efSearch;       /* Search beam width (e.g. 64) */
} pdb_options_t;

/**
 * @brief Populate default database options.
 */
void pdb_options_init(pdb_options_t* options);

/* ---------------------------------------------------------------------------
 * Database Lifecycle
 * --------------------------------------------------------------------------- */
/**
 * @brief Open database with default options.
 *
 * @param dbname File path to database directory.
 * @param embedder_type Embedder backend ("local" or "mock").
 * @param out_db Output pointer to allocated pdb_t handle.
 * @return PDB_OK on success, or negative error code.
 */
int pdb_open(const char* dbname, const char* embedder_type, pdb_t** out_db);

/**
 * @brief Open database with custom options.
 */
int pdb_open_with_options(const char* dbname,
                          const pdb_options_t* options,
                          const char* embedder_type,
                          pdb_t** out_db);

/**
 * @brief Flush and close database handle, releasing all allocated resources.
 */
void pdb_close(pdb_t* db);

/* ---------------------------------------------------------------------------
 * Query and Filtering
 * --------------------------------------------------------------------------- */
typedef struct pdb_filter_t {
  const char* video_id;  /* NULL or empty string for all videos */
  double start_sec;      /* < 0 for unbounded start */
  double end_sec;        /* < 0 for unbounded end */
} pdb_filter_t;

/**
 * @brief Search video moments by natural language text query.
 *
 * @param db Database handle.
 * @param text_query Natural language string query (UTF-8).
 * @param top_k Maximum number of results to return.
 * @param filter Optional metadata filter predicates (can be NULL).
 * @param out_results Output pointer to allocated pdb_results_t handle.
 * @return PDB_OK on success, or negative error code.
 */
int pdb_search_text(pdb_t* db,
                    const char* text_query,
                    int top_k,
                    const pdb_filter_t* filter,
                    pdb_results_t** out_results);

/**
 * @brief Search video moments by visual example image.
 *
 * @param db Database handle.
 * @param rgb_data Contiguous raw RGB pixel bytes (width * height * 3).
 * @param width Image width in pixels.
 * @param height Image height in pixels.
 * @param channels Number of color channels (must be 3).
 * @param top_k Maximum number of results to return.
 * @param filter Optional metadata filter predicates (can be NULL).
 * @param out_results Output pointer to allocated pdb_results_t handle.
 * @return PDB_OK on success, or negative error code.
 */
int pdb_search_image(pdb_t* db,
                     const uint8_t* rgb_data,
                     int width,
                     int height,
                     int channels,
                     int top_k,
                     const pdb_filter_t* filter,
                     pdb_results_t** out_results);

/* ---------------------------------------------------------------------------
 * Query Results Inspection
 * --------------------------------------------------------------------------- */
/**
 * @brief Returns total number of hit entries in the results handle.
 */
int pdb_results_count(const pdb_results_t* results);

/**
 * @brief Retrieve attributes for hit entry at given index.
 * String pointers remain valid until pdb_results_destroy() is called.
 */
int pdb_results_get(const pdb_results_t* results,
                    int index,
                    const char** out_video_id,
                    uint32_t* out_moment_id,
                    double* out_start_sec,
                    double* out_end_sec,
                    float* out_score,
                    const char** out_thumbnail_path,
                    const char** out_caption);

/**
 * @brief Free results handle and associated memory.
 */
void pdb_results_destroy(pdb_results_t* results);

/* ---------------------------------------------------------------------------
 * Video Ingestion and Mutations
 * --------------------------------------------------------------------------- */
/**
 * @brief Ingest video file from disk, returning content-addressed video_id.
 *
 * @param db Database handle.
 * @param video_path Absolute or relative path to video file.
 * @param out_video_id_buf Destination buffer for video_id string (null-terminated).
 * @param buf_len Size of destination buffer (recommended at least 65 bytes for SHA-256).
 * @return PDB_OK on success, or negative error code.
 */
int pdb_add_video(pdb_t* db,
                  const char* video_path,
                  char* out_video_id_buf,
                  size_t buf_len);

/**
 * @brief Delete video and all associated moments and vectors in O(1) tombstones.
 */
int pdb_delete_video(pdb_t* db, const char* video_id);

/* ---------------------------------------------------------------------------
 * Database Statistics
 * --------------------------------------------------------------------------- */
typedef struct pdb_stats_t {
  uint64_t num_videos;
  uint64_t num_moments;
  uint64_t num_vectors;
  uint64_t disk_size_bytes;
} pdb_stats_t;

/**
 * @brief Retrieve current database statistics.
 */
int pdb_get_stats(pdb_t* db, pdb_stats_t* out_stats);

#ifdef __cplusplus
}
#endif

#endif  /* PURPLEDB_C_H_ */
