// Runs the Godot web test build in Chrome and prints its console output.
// usage: node run.js <web-build-dir> <baked-dir> [test-case-filter]
const http = require('http');
const fs = require('fs');
const path = require('path');
const puppeteer = require('puppeteer-core');

const [webDir, bakedDir, filter = '*Browser*'] = process.argv.slice(2);
const CHROME = process.env.CHROME || 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe';
const TIMEOUT_MS = Number(process.env.TIMEOUT_MS || 180000);

const types = { '.js': 'text/javascript', '.wasm': 'application/wasm', '.html': 'text/html', '.json': 'application/json' };

const server = http.createServer((req, res) => {
	const url = decodeURIComponent(req.url.split('?')[0]);
	let file;
	if (url === '/' || url === '/index.html') {
		file = path.join(__dirname, 'index.html');
	} else if (url.startsWith('/baked/')) {
		file = path.join(bakedDir, url.slice('/baked/'.length));
	} else {
		file = path.join(webDir, url);
	}
	fs.readFile(file, (err, data) => {
		if (err) {
			res.writeHead(404);
			res.end('not found: ' + url);
			return;
		}
		res.writeHead(200, { 'Content-Type': types[path.extname(file)] || 'application/octet-stream' });
		res.end(data);
	});
});

(async () => {
	await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
	const port = server.address().port;
	const baked = fs.readdirSync(bakedDir).filter((f) => f.endsWith('.bin'));

	const browser = await puppeteer.launch({
		executablePath: CHROME,
		headless: process.env.HEADLESS === '0' ? false : 'new',
		args: ['--enable-unsafe-webgpu', '--enable-webgpu-developer-features', '--no-sandbox', '--ignore-gpu-blocklist'],
	});
	const page = await browser.newPage();
	let done = false;
	let status = null;
	const finish = (why) => {
		if (!done) {
			done = true;
			console.log('--- finished:', why);
		}
	};
	page.on('console', (msg) => {
		const text = msg.text();
		console.log(text);
		const m = text.match(/\[doctest\] Status: (\w+)/);
		if (m) {
			status = m[1];
			setTimeout(() => finish('doctest ' + status), 500);
		}
	});
	page.on('pageerror', (err) => console.log('PAGE ERROR:', err.message));
	page.on('requestfailed', (req) => console.log('REQUEST FAILED:', req.url()));

	const url = `http://127.0.0.1:${port}/?filter=${encodeURIComponent(filter)}&baked=${encodeURIComponent(baked.join(','))}`;
	await page.goto(url);
	const started = Date.now();
	while (!done && Date.now() - started < TIMEOUT_MS) {
		await new Promise((r) => setTimeout(r, 500));
	}
	if (!done) {
		console.log('--- TIMEOUT after', TIMEOUT_MS, 'ms');
	}
	if (process.env.SCREENSHOT) {
		await new Promise((r) => setTimeout(r, 1000)); // Let the last frame reach the screen.
		await page.screenshot({ path: process.env.SCREENSHOT });
		console.log('--- screenshot saved to', process.env.SCREENSHOT);
	}
	if (process.env.KEEP_OPEN === '1') {
		console.log('--- KEEP_OPEN=1: close the browser window to finish.');
		await new Promise((resolve) => browser.on('disconnected', resolve));
		server.close();
		process.exit(status === 'SUCCESS' ? 0 : 1);
	}
	await browser.close();
	server.close();
	process.exit(status === 'SUCCESS' ? 0 : 1);
})().catch((e) => {
	console.error(e);
	process.exit(2);
});
