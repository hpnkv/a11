set(A11_STORES_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/chunk_store_reader.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/chunk_store_writer.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/local_chunk_store.cc)
if (A11_BUILD_REDIS)
    list(APPEND A11_STORES_SOURCES
         ${CMAKE_CURRENT_SOURCE_DIR}/redis_chunk_store.cc)
endif ()
if (A11_BUILD_SQLITE)
    list(APPEND A11_STORES_SOURCES
         ${CMAKE_CURRENT_SOURCE_DIR}/internal/sqlite_database.cc
         ${CMAKE_CURRENT_SOURCE_DIR}/sqlite_chunk_store.cc)
endif ()
set(A11_STORES_SOURCES ${A11_STORES_SOURCES} PARENT_SCOPE)
