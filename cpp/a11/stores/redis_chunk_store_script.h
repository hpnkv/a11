// Copyright 2026 The A11 Authors.

#ifndef A11_STORES_REDIS_CHUNK_STORE_SCRIPT_H_
#define A11_STORES_REDIS_CHUNK_STORE_SCRIPT_H_

#include <string_view>

namespace a11::stores::internal {

// One dispatcher keeps every operation on the same versioned schema and the
// same explicitly declared key set. Redis Cluster can therefore verify that
// all touched keys share the caller-selected node slot.
inline constexpr std::string_view kRedisChunkStoreScript = R"lua(
local meta_key = KEYS[1]
local stream_key = KEYS[2]
local seq_key = KEYS[3]
local arrival_key = KEYS[4]
local blobs_key = KEYS[5]
local events_channel = KEYS[6]

local operation = ARGV[1]
local node_id = ARGV[2]
local max_seq_value = 4294967295
local max_count_value = 4294967296
-- At most 2^32 successful puts, 2^32 first-time clears, and one close can
-- publish a revision. Keeping every Lua counter below 2^53 also avoids the
-- lossy integer conversions of Redis's Lua 5.1 number type.
local max_revision_value = 8589934593

if type(operation) ~= 'string' or type(node_id) ~= 'string' or node_id == '' then
  return {'error', 'INVALID_ARGUMENT', 'Invalid chunk-store script envelope'}
end

local function failure(code, message)
  return {'error', code, message}
end

local function key_type(key)
  local reply = redis.call('TYPE', key)
  if type(reply) == 'table' then
    return reply['ok']
  end
  return reply
end

local function check_type(key, expected)
  local actual = key_type(key)
  return actual == 'none' or actual == expected
end

local function is_canonical_decimal(value, maximum)
  if type(value) ~= 'string' or value == '' or
      string.find(value, '[^0-9]') then
    return false
  end
  if #value > 1 and string.sub(value, 1, 1) == '0' then
    return false
  end
  if #value ~= #maximum then
    return #value < #maximum
  end
  return value <= maximum
end

local function canonical_uint(value, maximum, maximum_text)
  if not is_canonical_decimal(value, maximum_text) then
    return nil
  end
  local parsed = tonumber(value)
  if not parsed or parsed < 0 or parsed > maximum or
      math.floor(parsed) ~= parsed then
    return nil
  end
  return parsed
end

if not check_type(meta_key, 'hash') then
  return failure('DATA_LOSS', 'Chunk store metadata key is not a hash')
end
if not check_type(stream_key, 'stream') then
  return failure('DATA_LOSS', 'Chunk store data key is not a stream')
end
if not check_type(seq_key, 'hash') then
  return failure('DATA_LOSS', 'Chunk store sequence index is not a hash')
end
if not check_type(arrival_key, 'hash') then
  return failure('DATA_LOSS', 'Chunk store arrival index is not a hash')
end
if not check_type(blobs_key, 'hash') then
  return failure('DATA_LOSS', 'Chunk store blob key is not a hash')
end

local meta_exists = redis.call('EXISTS', meta_key) == 1
local metadata_closed = false
local metadata_size = 0
local metadata_put_count = 0
local metadata_next_cursor = 0
local metadata_revision = 0
local metadata_final_seq = nil
local metadata_max_seq = nil
if not meta_exists then
  if redis.call('EXISTS', stream_key, seq_key, arrival_key, blobs_key) ~= 0 then
    return failure('DATA_LOSS', 'Chunk data exists without store metadata')
  end
else
  local stored_id = redis.call('HGET', meta_key, 'id')
  local schema = redis.call('HGET', meta_key, 'schema')
  local closed = redis.call('HGET', meta_key, 'closed')
  local size = redis.call('HGET', meta_key, 'size')
  local put_count = redis.call('HGET', meta_key, 'put_count')
  local next_cursor = redis.call('HGET', meta_key, 'next_cursor')
  local revision = redis.call('HGET', meta_key, 'revision')
  local final_seq = redis.call('HGET', meta_key, 'final_seq')
  local max_seq = redis.call('HGET', meta_key, 'max_seq')
  metadata_size = canonical_uint(size, max_count_value, '4294967296')
  metadata_put_count = canonical_uint(
    put_count, max_count_value, '4294967296')
  metadata_next_cursor = canonical_uint(
    next_cursor, max_count_value, '4294967296')
  metadata_revision = canonical_uint(
    revision, max_revision_value, '8589934593')
  if final_seq then
    metadata_final_seq = canonical_uint(
      final_seq, max_seq_value, '4294967295')
  end
  if max_seq then
    metadata_max_seq = canonical_uint(max_seq, max_seq_value, '4294967295')
  end
  if not stored_id or schema ~= '1' or
      (closed ~= '0' and closed ~= '1') or metadata_size == nil or
      metadata_put_count == nil or metadata_next_cursor == nil or
      metadata_revision == nil or
      (final_seq and metadata_final_seq == nil) or
      (max_seq and metadata_max_seq == nil) then
    return failure('DATA_LOSS', 'Chunk store metadata is incomplete or corrupt')
  end
  if stored_id ~= node_id then
    return failure('DATA_LOSS', 'Chunk store key belongs to a different node')
  end
  metadata_closed = closed == '1'
  local stored_status = redis.call('HGET', meta_key, 'status')
  if metadata_closed and not stored_status then
    return failure('DATA_LOSS', 'Closed chunk store has no terminal status')
  end
  if not metadata_closed and stored_status then
    return failure('DATA_LOSS', 'Open chunk store has a terminal status')
  end
  if metadata_size ~= metadata_put_count or
      metadata_next_cursor > metadata_size then
    return failure('DATA_LOSS', 'Chunk store counters are inconsistent')
  end
  if (metadata_size == 0 and metadata_max_seq ~= nil) or
      (metadata_size > 0 and metadata_max_seq == nil) or
      (metadata_final_seq ~= nil and
       metadata_final_seq ~= metadata_max_seq) then
    return failure('DATA_LOSS', 'Chunk store sequence metadata is inconsistent')
  end
  if redis.call('HLEN', seq_key) ~= metadata_size or
      redis.call('HLEN', arrival_key) ~= metadata_put_count then
    return failure('DATA_LOSS', 'Chunk store index cardinality is inconsistent')
  end
  if redis.call('HLEN', blobs_key) > metadata_size then
    return failure('DATA_LOSS', 'Chunk store contains unindexed blobs')
  end
  local expected_stream_size = metadata_size + (metadata_closed and 1 or 0)
  if redis.call('XLEN', stream_key) ~= expected_stream_size then
    return failure('DATA_LOSS', 'Chunk store stream cardinality is inconsistent')
  end
end

local function ensure_meta()
  if not meta_exists then
    redis.call('HSET', meta_key,
      'schema', '1',
      'id', node_id,
      'closed', '0',
      'size', '0',
      'put_count', '0',
      'next_cursor', '0',
      'revision', '0')
    meta_exists = true
  end
end

local function revision()
  if not meta_exists then
    return '0'
  end
  return redis.call('HGET', meta_key, 'revision') or '0'
end

local function publish_change()
  -- Every caller checks this before its first mutation. HINCRBY therefore
  -- cannot overflow after a batch has partially committed.
  local changed = redis.call('HINCRBY', meta_key, 'revision', 1)
  redis.call('PUBLISH', events_channel, tostring(changed))
  return changed
end

local function can_publish_change()
  if metadata_revision >= max_revision_value then
    return failure('RESOURCE_EXHAUSTED',
      'Chunk store mutation revision exhausted')
  end
  return nil
end

local function field_map(entry)
  if #entry == 0 then
    return nil
  end
  local fields = entry[1][2]
  local result = {}
  for index = 1, #fields, 2 do
    result[fields[index]] = fields[index + 1]
  end
  return result
end

local function can_append_stream_entry()
  if redis.call('EXISTS', stream_key) == 0 then
    return true
  end
  local info = redis.call('XINFO', 'STREAM', stream_key)
  local stream_id = nil
  for index = 1, #info, 2 do
    if info[index] == 'last-generated-id' then
      stream_id = info[index + 1]
      break
    end
  end
  if not stream_id then
    return false
  end
  local separator = string.find(stream_id, '-', 1, true)
  if not separator then
    return false
  end
  -- XADD * may increment the sequence component, but no automatic ID can be
  -- greater once the millisecond component itself has reached uint64_t max.
  return string.sub(stream_id, 1, separator - 1) ~= '18446744073709551615'
end

-- Returns state, payload-or-ref, stream id, arrival, and storage. Every
-- payload is an encoded Chunk. Tombstones retain a separately prepared,
-- data-free encoding so ClearData can preserve metadata atomically without
-- teaching Lua A11's MessagePack schema.
local function load_chunk(seq)
  local stream_id = redis.call('HGET', seq_key, tostring(seq))
  if not stream_id then
    return 'missing', '', '', '', ''
  end
  local entry = redis.call('XRANGE', stream_key, stream_id, stream_id, 'COUNT', 1)
  local fields = field_map(entry)
  if not fields then
    return 'data_loss', 'Sequence index references a missing stream entry',
      stream_id, '', ''
  end
  if fields['v'] ~= '1' or fields['kind'] ~= 'chunk' or
      fields['seq'] ~= tostring(seq) or not fields['arrival'] then
    return 'data_loss', 'Stream entry metadata is corrupt', stream_id, '', ''
  end
  local arrival = canonical_uint(
    fields['arrival'], max_seq_value, '4294967295')
  if arrival == nil or
      redis.call('HGET', arrival_key, fields['arrival']) ~= tostring(seq) then
    return 'data_loss', 'Stream entry arrival index is corrupt', stream_id,
      fields['arrival'], ''
  end
  local storage = fields['storage']
  if storage == 'inline' then
    if fields['payload'] == nil then
      return 'data_loss', 'Inline stream entry has no payload', stream_id,
        fields['arrival'], storage
    end
    return 'item', fields['payload'], stream_id, fields['arrival'], storage
  end
  if storage == 'redis' then
    if fields['ref'] ~= tostring(seq) then
      return 'data_loss', 'Redis stream entry has no blob reference', stream_id,
        fields['arrival'], storage
    end
    local payload = redis.call('HGET', blobs_key, fields['ref'])
    if payload == false then
      return 'data_loss', 'Redis stream entry references a missing blob',
        stream_id, fields['arrival'], storage
    end
    return 'item', payload, stream_id, fields['arrival'], storage
  end
  if storage == 'tombstone' then
    if fields['payload'] == nil then
      return 'data_loss', 'Tombstone stream entry has no payload', stream_id,
        fields['arrival'], storage
    end
    return 'item', fields['payload'], stream_id, fields['arrival'], storage
  end
  if storage == 's3' then
    return 's3', fields['ref'] or '', stream_id, fields['arrival'], storage
  end
  return 'data_loss', 'Stream entry has an unknown storage kind', stream_id,
    fields['arrival'], storage or ''
end

local function final_seq()
  if not meta_exists then
    return ''
  end
  return redis.call('HGET', meta_key, 'final_seq') or ''
end

local function missing_result(kind, value)
  if meta_exists and redis.call('HGET', meta_key, 'closed') == '1' then
    return {'closed', redis.call('HGET', meta_key, 'status')}
  end
  return {'wait', revision(), kind, tostring(value)}
end

if operation == 'initialize' then
  if #ARGV ~= 2 then
    return failure('INVALID_ARGUMENT', 'Invalid initialize arguments')
  end
  ensure_meta()
  return {'ok'}
end

if operation == 'put' then
  if metadata_closed then
    return failure('FAILED_PRECONDITION', 'Chunk store is closed for writes')
  end
  local count = canonical_uint(ARGV[3], max_count_value, '4294967296')
  if count == nil then
    return failure('INVALID_ARGUMENT', 'Invalid fragment count')
  end
  if #ARGV ~= 3 + count * 5 then
    return failure('INVALID_ARGUMENT', 'Fragment count does not match arguments')
  end
  if count == 0 then
    return {'ok'}
  end
  if metadata_put_count + count > max_count_value then
    return failure('RESOURCE_EXHAUSTED',
      'Maximum chunk-store cardinality exceeded')
  end
  local explicit = ARGV[4] ~= ''
  local put_count = metadata_put_count
  local candidate = put_count
  local assigned = {}
  local seen = {}
  local batch_final = nil
  local saw_final = false
  local pending_max = metadata_max_seq

  for index = 1, count do
    local base = 4 + (index - 1) * 5
    local supplied_seq = ARGV[base]
    local is_final = ARGV[base + 1]
    local storage = ARGV[base + 2]
    local payload = ARGV[base + 3]
    local tombstone = ARGV[base + 4]
    if (supplied_seq ~= '') ~= explicit then
      return failure('INVALID_ARGUMENT',
        'Sequence numbers must be set on every fragment or none')
    end
    local seq = nil
    if explicit then
      seq = canonical_uint(supplied_seq, max_seq_value, '4294967295')
      if seq == nil then
        return failure('INVALID_ARGUMENT', 'Invalid explicit sequence number')
      end
    else
      while candidate <= max_seq_value and
          redis.call('HEXISTS', seq_key, tostring(candidate)) == 1 do
        candidate = candidate + 1
      end
      if candidate > max_seq_value then
        return failure('RESOURCE_EXHAUSTED',
          'Maximum implicit sequence number exceeded')
      end
      seq = candidate
      candidate = candidate + 1
    end
    local seq_text = tostring(seq)
    if seen[seq_text] then
      return failure('INVALID_ARGUMENT',
        'A sequence occurs more than once in the batch')
    end
    seen[seq_text] = true
    if redis.call('HEXISTS', seq_key, seq_text) == 1 then
      return failure('ALREADY_EXISTS',
        'A fragment with seq ' .. seq_text .. ' already exists')
    end
    if redis.call('HEXISTS', blobs_key, seq_text) == 1 then
      return failure('DATA_LOSS',
        'An unindexed blob collides with seq ' .. seq_text)
    end
    local arrival = tostring(put_count + index - 1)
    if redis.call('HEXISTS', arrival_key, arrival) == 1 then
      return failure('DATA_LOSS',
        'Arrival index ' .. arrival .. ' already exists')
    end
    if storage ~= 'inline' and storage ~= 'redis' then
      return failure('INVALID_ARGUMENT', 'Invalid chunk storage kind')
    end
    if payload == nil then
      return failure('INVALID_ARGUMENT', 'Chunk payload is missing')
    end
    if tombstone == nil then
      return failure('INVALID_ARGUMENT', 'Chunk tombstone payload is missing')
    end
    if is_final ~= '0' and is_final ~= '1' then
      return failure('INVALID_ARGUMENT', 'Invalid final-fragment marker')
    end
    if is_final == '1' then
      if saw_final then
        return failure('INVALID_ARGUMENT',
          'More than one fragment in the batch is marked final')
      end
      if not explicit and index ~= count then
        return failure('INVALID_ARGUMENT',
          'The final implicit fragment must be last')
      end
      saw_final = true
      batch_final = seq
    end
    assigned[index] = seq
    if not pending_max or seq > pending_max then
      pending_max = seq
    end
  end

  local existing_final = metadata_final_seq
  if batch_final and existing_final and batch_final ~= existing_final then
    return failure('FAILED_PRECONDITION',
      'The chunk store already has a different final sequence')
  end
  local pending_final = batch_final or existing_final
  if pending_final then
    local existing_max = metadata_max_seq
    if existing_max and existing_max > pending_final then
      return failure('INVALID_ARGUMENT',
        'An existing fragment exceeds the proposed final sequence')
    end
    for index = 1, count do
      if assigned[index] > pending_final then
        return failure('INVALID_ARGUMENT',
          'A fragment sequence exceeds the final sequence')
      end
    end
  end

  local revision_error = can_publish_change()
  if revision_error then
    return revision_error
  end
  if not can_append_stream_entry() then
    return failure('RESOURCE_EXHAUSTED', 'Redis Stream ID space is exhausted')
  end
  ensure_meta()
  local response = {'ok'}
  for index = 1, count do
    local base = 4 + (index - 1) * 5
    local storage = ARGV[base + 2]
    local payload = ARGV[base + 3]
    local tombstone = ARGV[base + 4]
    local seq_text = tostring(assigned[index])
    local arrival = tostring(put_count + index - 1)
    local stream_id = nil
    if storage == 'redis' then
      redis.call('HSET', blobs_key, seq_text, payload)
      stream_id = redis.call('XADD', stream_key, '*',
        'v', '1', 'kind', 'chunk', 'seq', seq_text, 'arrival', arrival,
        'storage', 'redis', 'ref', seq_text, 'tombstone', tombstone)
    else
      stream_id = redis.call('XADD', stream_key, '*',
        'v', '1', 'kind', 'chunk', 'seq', seq_text, 'arrival', arrival,
        'storage', 'inline', 'payload', payload, 'tombstone', tombstone)
    end
    redis.call('HSET', seq_key, seq_text, stream_id)
    redis.call('HSET', arrival_key, arrival, seq_text)
    response[#response + 1] = seq_text
  end
  redis.call('HINCRBY', meta_key, 'size', count)
  redis.call('HINCRBY', meta_key, 'put_count', count)
  redis.call('HSET', meta_key, 'max_seq', tostring(pending_max))
  if batch_final then
    redis.call('HSET', meta_key, 'final_seq', tostring(batch_final))
  end
  publish_change()
  return response
end

if operation == 'lookup' then
  if #ARGV ~= 4 then
    return failure('INVALID_ARGUMENT', 'Invalid lookup arguments')
  end
  local kind = ARGV[3]
  local value = ARGV[4]
  local seq_text = value
  if kind == 'arrival' then
    if not is_canonical_decimal(value, '18446744073709551615') then
      return failure('INVALID_ARGUMENT', 'Invalid arrival-order lookup')
    end
    seq_text = redis.call('HGET', arrival_key, value)
    if not seq_text then
      return missing_result(kind, value)
    end
  elseif kind ~= 'sequence' then
    return failure('INVALID_ARGUMENT', 'Invalid Redis chunk lookup kind')
  end
  local seq = canonical_uint(seq_text, max_seq_value, '4294967295')
  if seq == nil then
    if kind == 'arrival' then
      return failure('DATA_LOSS', 'Arrival index contains an invalid sequence')
    end
    return failure('INVALID_ARGUMENT', 'Invalid lookup sequence')
  end
  local state, value_or_error, _, _, storage = load_chunk(seq)
  if state == 'missing' then
    return missing_result(kind, value)
  end
  if state == 'data_loss' then
    return failure('DATA_LOSS', value_or_error)
  end
  if state == 's3' then
    return {'item', seq_text, 's3', value_or_error, final_seq()}
  end
  return {'item', seq_text, storage, value_or_error, final_seq()}
end

if operation == 'next' then
  if #ARGV ~= 3 then
    return failure('INVALID_ARGUMENT', 'Invalid next arguments')
  end
  local limit = canonical_uint(ARGV[3], 1024, '1024')
  if limit == nil or limit == 0 then
    return failure('INVALID_ARGUMENT', 'limit must be positive')
  end
  if not meta_exists then
    return {'next', 'wait', '', '0'}
  end
  local cursor = metadata_next_cursor
  local final_text = final_seq()
  local final = metadata_final_seq
  local items = {}
  local item_count = 0
  local disposition = nil
  local detail = ''

  while true do
    -- LocalChunkStore treats exhausting the uint32_t sequence namespace as an
    -- end sentinel even when no final fragment or close status was recorded.
    if cursor > max_seq_value then
      disposition = 'end'
      break
    end
    if final and cursor > final then
      if redis.call('HGET', meta_key, 'closed') == '1' then
        disposition = 'closed'
        detail = redis.call('HGET', meta_key, 'status')
      else
        disposition = 'end'
      end
      break
    end
    if item_count == limit then
      disposition = 'ready'
      break
    end
    local state, value_or_error, _, _, storage = load_chunk(cursor)
    if state == 'missing' then
      if redis.call('HGET', meta_key, 'closed') == '1' then
        disposition = 'closed'
        detail = redis.call('HGET', meta_key, 'status')
      else
        disposition = 'wait'
      end
      break
    end
    if state == 'data_loss' then
      disposition = 'data_loss'
      detail = value_or_error
      break
    end
    if state == 's3' then
      disposition = 's3'
      detail = value_or_error
      break
    end
    item_count = item_count + 1
    items[#items + 1] = tostring(cursor)
    items[#items + 1] = storage
    items[#items + 1] = value_or_error
    items[#items + 1] = final_text
    cursor = cursor + 1
  end

  if item_count > 0 and disposition ~= 'data_loss' and disposition ~= 's3' then
    redis.call('HSET', meta_key, 'next_cursor', tostring(cursor))
  end
  local response = {'next', disposition, detail, tostring(item_count)}
  for index = 1, #items do
    response[#response + 1] = items[index]
  end
  return response
end

if operation == 'clear' then
  if #ARGV ~= 3 then
    return failure('INVALID_ARGUMENT', 'Invalid clear arguments')
  end
  local seq_text = ARGV[3]
  local seq = canonical_uint(seq_text, max_seq_value, '4294967295')
  if seq == nil then
    return failure('INVALID_ARGUMENT', 'Invalid sequence to clear')
  end
  local state, value_or_error, stream_id, arrival, storage = load_chunk(seq)
  if state == 'missing' then
    return failure('NOT_FOUND', 'No fragment with seq ' .. seq_text .. ' exists')
  end
  if state == 'data_loss' then
    return failure('DATA_LOSS', value_or_error)
  end
  if state == 's3' then
    return failure('UNIMPLEMENTED',
      'Clearing S3-backed chunks is not implemented')
  end
  if storage == 'tombstone' then
    return {'item', seq_text, 'tombstone', value_or_error, final_seq()}
  end
  local entry = redis.call('XRANGE', stream_key, stream_id, stream_id,
    'COUNT', 1)
  local fields = field_map(entry)
  if not fields or fields['tombstone'] == nil then
    return failure('DATA_LOSS',
      'Stream entry has no prepared tombstone payload')
  end
  local revision_error = can_publish_change()
  if revision_error then
    return revision_error
  end
  if not can_append_stream_entry() then
    return failure('RESOURCE_EXHAUSTED', 'Redis Stream ID space is exhausted')
  end
  local tombstone = fields['tombstone']
  -- Append first: XADD is the only remaining command that can reject valid
  -- arguments because of stream-ID state. No old payload has been removed if
  -- that happens.
  local tombstone_id = redis.call('XADD', stream_key, '*',
    'v', '1', 'kind', 'chunk', 'seq', seq_text, 'arrival', arrival,
    'storage', 'tombstone', 'payload', tombstone,
    'tombstone', tombstone)
  if storage == 'redis' then
    redis.call('HDEL', blobs_key, seq_text)
  end
  redis.call('XDEL', stream_key, stream_id)
  redis.call('HSET', seq_key, seq_text, tombstone_id)
  publish_change()
  return {'item', seq_text, storage, value_or_error, final_seq()}
end

if operation == 'arrival_seq' then
  if #ARGV ~= 3 then
    return failure('INVALID_ARGUMENT', 'Invalid arrival lookup arguments')
  end
  if not is_canonical_decimal(ARGV[3], '18446744073709551615') then
    return failure('INVALID_ARGUMENT', 'Invalid arrival order')
  end
  local seq = redis.call('HGET', arrival_key, ARGV[3])
  if not seq then
    return failure('NOT_FOUND',
      'No fragment has arrival order ' .. ARGV[3])
  end
  if canonical_uint(seq, max_seq_value, '4294967295') == nil or
      redis.call('HEXISTS', seq_key, seq) ~= 1 then
    return failure('DATA_LOSS',
      'Arrival index references an invalid or missing sequence')
  end
  return {'value', seq}
end

if operation == 'final' then
  if #ARGV ~= 2 then
    return failure('INVALID_ARGUMENT', 'Invalid final-sequence arguments')
  end
  return {'optional', final_seq()}
end

if operation == 'size' then
  if #ARGV ~= 2 then
    return failure('INVALID_ARGUMENT', 'Invalid size arguments')
  end
  if not meta_exists then
    return {'value', '0'}
  end
  return {'value', redis.call('HGET', meta_key, 'size')}
end

if operation == 'close' then
  if #ARGV ~= 4 then
    return failure('INVALID_ARGUMENT', 'Invalid close arguments')
  end
  local requested_status = ARGV[3]
  local return_existing = ARGV[4]
  if requested_status == nil or
      (return_existing ~= '0' and return_existing ~= '1') then
    return failure('INVALID_ARGUMENT', 'Invalid chunk-store close arguments')
  end
  if metadata_closed then
    if return_existing == '1' then
      return {'status', redis.call('HGET', meta_key, 'status')}
    end
    return failure('FAILED_PRECONDITION',
      'Chunk store is already closed for writes')
  end
  local revision_error = can_publish_change()
  if revision_error then
    return revision_error
  end
  if not can_append_stream_entry() then
    return failure('RESOURCE_EXHAUSTED', 'Redis Stream ID space is exhausted')
  end
  local changed = metadata_revision + 1
  -- As in ClearData, append the stream transition before making any other
  -- mutation so an invalid stream-ID state cannot leave a half-closed store.
  redis.call('XADD', stream_key, '*', 'v', '1', 'kind', 'control',
    'event', 'close', 'revision', tostring(changed))
  ensure_meta()
  redis.call('HSET', meta_key, 'closed', '1', 'status', requested_status)
  redis.call('HINCRBY', meta_key, 'revision', 1)
  redis.call('PUBLISH', events_channel, tostring(changed))
  return {'status', requested_status}
end

if operation == 'metadata' then
  if #ARGV ~= 2 then
    return failure('INVALID_ARGUMENT', 'Invalid metadata arguments')
  end
  if not meta_exists then
    return {'metadata', node_id, '0', '', '', '0', '0', '0', '', '0'}
  end
  local closed = redis.call('HGET', meta_key, 'closed')
  return {'metadata',
    redis.call('HGET', meta_key, 'id'),
    closed,
    closed == '1' and redis.call('HGET', meta_key, 'status') or '',
    redis.call('HGET', meta_key, 'final_seq') or '',
    redis.call('HGET', meta_key, 'size'),
    redis.call('HGET', meta_key, 'put_count'),
    redis.call('HGET', meta_key, 'next_cursor'),
    redis.call('HGET', meta_key, 'max_seq') or '',
    redis.call('HGET', meta_key, 'revision')}
end

return failure('INVALID_ARGUMENT', 'Unknown chunk store operation')
)lua";

}  // namespace a11::stores::internal

#endif  // A11_STORES_REDIS_CHUNK_STORE_SCRIPT_H_
