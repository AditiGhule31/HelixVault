const http = require('http');
const fs = require('fs');
const path = require('path');
const url = require('url');

// ==========================================
// STORAGE ENGINE - Hash Table (like C code)
// ==========================================
const HASH_TABLE_SIZE = 100;
const disk = new Array(HASH_TABLE_SIZE).fill(null);
let totalSpaceSaved = 0;
let uniqueFilesCount = 0;

function djb2Hash(str) {
  let hash = 5381n;
  for (let i = 0; i < str.length; i++) {
    hash = ((hash << 5n) + hash) + BigInt(str.charCodeAt(i));
    hash = hash & 0xFFFFFFFFFFFFFFFFn;
  }
  return hash;
}

function processUpload(content, user, filePath) {
  const hashVal = djb2Hash(content);
  const outHash = hashVal.toString();
  const index = Number(hashVal % BigInt(HASH_TABLE_SIZE));

  let curr = disk[index];
  while (curr !== null) {
    if (curr.fileHash === outHash) {
      curr.owners.push(user);
      curr.refCount++;
      totalSpaceSaved += content.length;
      insertPath(filePath, outHash);
      return { isDuplicate: true, hash: outHash };
    }
    curr = curr.next;
  }

  const newBlock = {
    content,
    fileHash: outHash,
    refCount: 1,
    owners: [user],
    next: disk[index],
    size: content.length
  };
  disk[index] = newBlock;
  uniqueFilesCount++;
  insertPath(filePath, outHash);
  return { isDuplicate: false, hash: outHash };
}

function getAllFiles() {
  const files = [];
  for (let i = 0; i < HASH_TABLE_SIZE; i++) {
    let curr = disk[i];
    while (curr) {
      files.push({
        hash: curr.fileHash,
        refCount: curr.refCount,
        owners: curr.owners,
        size: curr.size,
        content: curr.content.substring(0, 100)
      });
      curr = curr.next;
    }
  }
  return files;
}

// ==========================================
// FILE SYSTEM - Trie (like C code)
// ==========================================
let trieRoot = { children: {}, isFile: false, fileHash: null };

function insertPath(filePath, hash) {
  const parts = filePath.split(/[/\\]/).filter(Boolean);
  let curr = trieRoot;
  for (let i = 0; i < parts.length; i++) {
    const part = parts[i];
    if (!curr.children[part]) {
      curr.children[part] = { children: {}, isFile: false, fileHash: null, name: part };
    }
    curr = curr.children[part];
  }
  curr.isFile = true;
  curr.fileHash = hash;
}

function getTrieTree(node, name = 'root') {
  const result = { name, isFile: node.isFile, fileHash: node.fileHash, children: [] };
  for (const [childName, childNode] of Object.entries(node.children)) {
    result.children.push(getTrieTree(childNode, childName));
  }
  return result;
}

// ==========================================
// HTTP SERVER
// ==========================================
const server = http.createServer((req, res) => {
  const parsed = url.parse(req.url, true);
  const pathname = parsed.pathname;

  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type');

  if (req.method === 'OPTIONS') {
    res.writeHead(200);
    res.end();
    return;
  }

  // Serve static files
  if (req.method === 'GET' && (pathname === '/' || pathname === '/index.html')) {
    const filePath = path.join(__dirname, 'index.html');
    fs.readFile(filePath, (err, data) => {
      if (err) { res.writeHead(404); res.end('Not found'); return; }
      res.writeHead(200, { 'Content-Type': 'text/html' });
      res.end(data);
    });
    return;
  }

  // API: Upload file
  if (req.method === 'POST' && pathname === '/api/upload') {
    let body = '';
    req.on('data', chunk => body += chunk);
    req.on('end', () => {
      try {
        const { user, filePath, content } = JSON.parse(body);
        if (!user || !filePath || !content) {
          res.writeHead(400, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify({ error: 'Missing fields' }));
          return;
        }
        const result = processUpload(content, user, filePath);
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({
          success: true,
          isDuplicate: result.isDuplicate,
          hash: result.hash,
          spaceSaved: totalSpaceSaved,
          uniqueFiles: uniqueFilesCount
        }));
      } catch (e) {
        res.writeHead(500, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ error: e.message }));
      }
    });
    return;
  }

  // API: Get all files
  if (req.method === 'GET' && pathname === '/api/files') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(getAllFiles()));
    return;
  }

  // API: Get trie tree
  if (req.method === 'GET' && pathname === '/api/trie') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(getTrieTree(trieRoot)));
    return;
  }

  // API: Get stats
  if (req.method === 'GET' && pathname === '/api/stats') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({
      uniqueFiles: uniqueFilesCount,
      totalSpaceSaved,
      deduplicationRatio: uniqueFilesCount > 0 ? ((totalSpaceSaved / (totalSpaceSaved + disk.reduce((sum, b) => {
        let curr = b, s = 0;
        while (curr) { s += curr.size; curr = curr.next; }
        return sum + s;
      }, 0))) * 100).toFixed(1) : 0
    }));
    return;
  }

  res.writeHead(404);
  res.end('Not found');
});

const PORT = 3000;
server.listen(PORT, () => {
  console.log(`\n✅ Cloud Storage Manager is running!`);
  console.log(`🌐 Open your browser at: http://localhost:${PORT}`);
  console.log(`\nPress Ctrl+C to stop the server.\n`);
});
