// Phase 0, day 5 spike (PRD §9): SQLite amalgamation + sqlite-vec, statically linked,
// with 10k synthetic 1280-dim vectors (mira's real embedding size, PRD §3 correction —
// not the 512-dim figure originally measured). Exact brute-force KNN, no index tuning
// (PRD §3, §7): "No ANN index. No FAISS, no hnswlib, no vector database."

#include <iostream>
#include <vector>
#include <random>
#include <chrono>
#include <cstring>
#include <sqlite3.h>
#include "sqlite-vec.h"

static constexpr int EMBED_DIM = 1280;
static constexpr int N_VECTORS = 10000;
static constexpr int TOP_K = 20;

static void check(int rc, sqlite3* db, const char* what) {
  if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
    std::cerr << what << " failed: " << sqlite3_errmsg(db) << std::endl;
    std::exit(1);
  }
}

int main() {
  sqlite3* db;
  // sqlite-vec must be registered as an auto-extension before opening any
  // connection when statically linked (SQLITE_VEC_STATIC), per its README.
  sqlite3_auto_extension((void (*)())sqlite3_vec_init);

  check(sqlite3_open(":memory:", &db), db, "open");

  check(sqlite3_exec(db,
      "CREATE VIRTUAL TABLE embeddings USING vec0(embedding float[1280])",
      nullptr, nullptr, nullptr), db, "create vec0 table");

  std::mt19937 rng(42);
  std::normal_distribution<float> dist(0.0f, 1.0f);

  std::cout << "inserting " << N_VECTORS << " synthetic " << EMBED_DIM << "-dim vectors..." << std::endl;
  auto insertStart = std::chrono::steady_clock::now();

  sqlite3_stmt* insertStmt;
  check(sqlite3_prepare_v2(db, "INSERT INTO embeddings(rowid, embedding) VALUES (?, ?)",
                            -1, &insertStmt, nullptr), db, "prepare insert");

  check(sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr), db, "begin");
  std::vector<float> vec(EMBED_DIM);
  std::vector<float> queryVec;
  for (int i = 0; i < N_VECTORS; ++i) {
    for (auto& v : vec) v = dist(rng);
    if (i == N_VECTORS / 2) queryVec = vec; // pick a real inserted vector as the query

    sqlite3_bind_int64(insertStmt, 1, i);
    sqlite3_bind_blob(insertStmt, 2, vec.data(), EMBED_DIM * sizeof(float), SQLITE_TRANSIENT);
    check(sqlite3_step(insertStmt), db, "step insert");
    sqlite3_reset(insertStmt);
  }
  check(sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr), db, "commit");
  sqlite3_finalize(insertStmt);

  auto insertEnd = std::chrono::steady_clock::now();
  double insertMs = std::chrono::duration<double, std::milli>(insertEnd - insertStart).count();
  std::cout << "insert: " << insertMs << " ms (" << (insertMs / N_VECTORS) << " ms/vector)" << std::endl;

  // --- KNN query: top-20 nearest to a known-inserted vector -----------------
  sqlite3_stmt* queryStmt;
  check(sqlite3_prepare_v2(db,
      "SELECT rowid, distance FROM embeddings "
      "WHERE embedding MATCH ? AND k = ? ORDER BY distance",
      -1, &queryStmt, nullptr), db, "prepare query");

  sqlite3_bind_blob(queryStmt, 1, queryVec.data(), EMBED_DIM * sizeof(float), SQLITE_TRANSIENT);
  sqlite3_bind_int(queryStmt, 2, TOP_K);

  auto queryStart = std::chrono::steady_clock::now();
  std::vector<std::pair<int64_t, double>> results;
  while (sqlite3_step(queryStmt) == SQLITE_ROW) {
    results.emplace_back(sqlite3_column_int64(queryStmt, 0), sqlite3_column_double(queryStmt, 1));
  }
  auto queryEnd = std::chrono::steady_clock::now();
  double queryMs = std::chrono::duration<double, std::milli>(queryEnd - queryStart).count();

  sqlite3_finalize(queryStmt);

  std::cout << "top-" << TOP_K << " query: " << queryMs << " ms, " << results.size() << " results" << std::endl;
  std::cout << "closest match: rowid=" << results.front().first
            << " distance=" << results.front().second
            << " (expected rowid=" << (N_VECTORS / 2) << ", distance=0)" << std::endl;

  bool selfMatchCorrect = !results.empty() && results.front().first == N_VECTORS / 2
                           && results.front().second < 1e-4;

  sqlite3_close(db);

  if (!selfMatchCorrect) {
    std::cerr << "SPIKE FAILED — self-match did not come back as the closest result" << std::endl;
    return 1;
  }

  std::cout << "SPIKE OK — sqlite-vec statically linked, KNN over 10k x 1280-dim vectors works." << std::endl;
  return 0;
}
