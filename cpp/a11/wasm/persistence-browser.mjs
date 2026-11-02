// Copyright 2026 The A11 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

export function indexedDbSnapshotStore(name, key = "snapshot") {
  const database = new Promise((resolve, reject) => {
    const request = indexedDB.open(name, 1);
    request.onupgradeneeded = () => request.result.createObjectStore("a11");
    request.onerror = () => reject(request.error);
    request.onsuccess = () => resolve(request.result);
  });

  function transact(mode, operation) {
    return database.then((db) => new Promise((resolve, reject) => {
      const transaction = db.transaction("a11", mode);
      const request = operation(transaction.objectStore("a11"));
      let value;
      request.onerror = () => reject(request.error);
      request.onsuccess = () => { value = request.result; };
      transaction.onerror = () => reject(transaction.error);
      transaction.onabort = () => reject(transaction.error);
      transaction.oncomplete = () => resolve(value);
    }));
  }

  return {
    async load() {
      const value = await transact("readonly", (store) => store.get(key));
      return value === undefined ? null : new Uint8Array(value);
    },
    async save(bytes) {
      await transact("readwrite", (store) => store.put(bytes.slice().buffer, key));
    },
    async close() {
      (await database).close();
    },
  };
}
