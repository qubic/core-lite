# Ant solution check

Two HTTP endpoints on the lite node, for the guardian team. Use them to see what a node stored
for a solution, and to make a node score a solution in front of you.

A node that takes scores on trust cannot answer the second one. That is the point.

Examples use `http://NODE:41841`. All calls are GET.

## 1. Look up stored solutions

```
GET /v1/ant-solutions
```

Give it a `tick` or an `identity` (or both).

| Parameter | Meaning |
|---|---|
| `tick` | solutions stored in that tick |
| `index` | only the one at this position in the tick |
| `identity` | solutions of this miner (60 letters) |
| `nonce` | only the one with this nonce (64 hex digits) |
| `limit` | how many to return, 1 to 64, default 64 |
| `offset` | skip this many matches first |
| `recordIndexFrom` | start scanning the store at this position |

```
curl "http://NODE:41841/v1/ant-solutions?tick=92260029"
```

```json
{
  "epoch": 241,
  "nodeVerifiesScores": true,
  "nextRecordIndexFrom": 0,
  "solutions": [
    {
      "recordIndex": 2,
      "identity": "KENGZYMYWOIHSCXMGBIXBGTKZYCCDITKSNBNILSLUFPQPRCUUYENPYUCEXRM",
      "nonce": "0125005480e7256989a47f54c630b2a60233015aa7ba305c98e5ceb65faff457",
      "tick": 92260029,
      "index": 0,
      "parentTick": 92260014,
      "parentIndex": 0,
      "anchorTick": 92260018,
      "depth": 2,
      "score": 4435,
      "shift": 0,
      "networkHash": 1956556149,
      "networkHeld": true,
      "trustedAtCommit": false
    }
  ]
}
```

What to look at:

- `nodeVerifiesScores`: false means this node trusts claimed scores (AUX without `--fv`).
- `trustedAtCommit`: true means the node stored this solution without walking it.
- `networkHeld`: false means the node has no network for it, so it cannot score its children.
- `tick` + `index` is the solution's address. A child points at its parent with `parentTick` + `parentIndex`.
  A first-generation solution has `parentTick` 0 and `parentIndex` 4294967295.
- `recordIndex` is just the position in this node's store. It restarts every epoch and can differ between nodes.

Paging: either raise `offset` by `limit` each call, or pass `nextRecordIndexFrom` back as
`recordIndexFrom`. Do not do both. `nextRecordIndexFrom` is 0 when there is nothing left.

Only accepted solutions are stored. A rejected one gives an empty list.

## 2. Make the node score a solution

```
GET /v1/ant-score
```

All six parameters are required.

| Parameter | Meaning |
|---|---|
| `identity` | the miner's identity, not yours |
| `nonce` | 64 hex digits |
| `anchorTick` | the tick the solution is anchored to |
| `parentTick`, `parentIndex` | address of the parent (0 and 4294967295 for no parent) |
| `signature` | 128 hex digits, see below |

How to sign:

```
message   = publicKey[32] | nonce[32] | anchorTick | parentTick | parentIndex     (76 bytes)
            the three numbers are 32-bit little-endian
digest    = KangarooTwelve(message), 32 bytes
signature = sign(guardian seed, digest)
```

Reply when it worked:

```json
{ "status": "ok", "score": 4602, "shift": 0, "networkHash": 2923967924, "cached": false, "walkMs": 27871 }
```

`cached: true` means the node already had this result and did not walk again. `walkMs` is only
there when it walked. The call returns when the walk is done, so give it time (seconds to tens of
seconds, depending on the CPU).

Other replies:

| `status` | HTTP | Meaning |
|---|---|---|
| `no-score` | 200 | walked but no valid score (for example it did not beat the parent), or the nonce is not valid |
| `node-trusts-scores` | 200 | this node does not verify scores, it computed nothing |
| `anchor-unavailable` | 200 | the node does not have that tick's digest (empty tick, too old, or in the future) |
| `parent-unknown` | 200 | no stored solution at that parent address |
| `wrong-tree` | 200 | the parent belongs to another miner |
| `parent-network-not-held` | 200 | the node stored the parent without its network |
| `no-price-series` | 200 | the node has no BTC price series |
| `busy` | 429 | another score request is running, try again |
| `bad-signature` | 401 | signature does not match the request or the guardian key |
| `bad-request` | 400 | a parameter is missing or malformed |
| `disabled` | 503 | no guardian identity in this build |

## The script

`check.ts` does the signing and paging for you. It needs [Bun](https://bun.sh).

```
cd tools/ant_solution_check
bun install
export GUARDIAN_SEED=<55 lowercase letters>

bun check.ts tree    --node http://NODE:41841 --identity MINER_ID
bun check.ts tick    --node http://NODE:41841 --tick 92260029
bun check.ts recheck --node http://NODE:41841 --tick 92260029 --index 0
bun check.ts verify  --node http://NODE:41841 --identity MINER_ID --parent-tick 92260014 --parent-index 0
```

- `tree` and `tick` list stored solutions.
- `recheck` takes a stored solution, asks the node to score it again and compares. It prints `match` or `MISMATCH`.
- `verify` sends a fresh random nonce as a child of the parent you name. It picks a recent anchor tick itself.
  Add `--nonce` and `--anchor-tick` to repeat an exact request.

## Good to know

- The guardian identity is compiled in: `ANT_CHECK_GUARDIAN_IDENTITY` in `src/extensions/ant_solution_check.h`.
  Empty means `/v1/ant-score` answers `disabled`.
- A node usually has its own solutions in cache, so `recheck` often comes back with `cached: true`.
  To force a real walk use `verify`: a fresh nonce cannot be in any cache.
- To catch a node that skips walking: pick a recent stored solution as parent and run `verify`.
  A node that really holds the parent's network answers `ok` or `no-score` with a `walkMs`.
  A trusting node answers `node-trusts-scores`. A node that trusted that parent earlier and never
  walked it answers `parent-network-not-held`.
- One score request runs at a time. It uses its own scoring engine at low priority, so it does not hold up the node.
- Stored solutions are gone after the epoch ends.
