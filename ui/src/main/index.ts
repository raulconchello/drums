// Electron main: window, engine process (spawn if needed), exercise listing, Omarchy theme, screenshots.
//
// Flags: --root <repo>  --port <n>  --screenshot <file.png>  --screenshot-wait <seconds>  --size <WxH>  --theme dark|omarchy
import { app, BrowserWindow, dialog, ipcMain, Menu } from 'electron';
import { spawn, type ChildProcess } from 'node:child_process';
import { existsSync, mkdirSync, readdirSync, readFileSync, writeFileSync } from 'node:fs';
import { createConnection } from 'node:net';
import { homedir } from 'node:os';
import { dirname, join, relative, resolve } from 'node:path';
import { stripJsonComments } from '../shared/exercise';
import type { AppInfo, EngineProcessStatus, ExerciseEntry, OmarchyTheme } from '../preload/api';

// Wayland when available (X11/XWayland otherwise). Also passed on the command line by the npm scripts,
// since some Chromium builds read the ozone platform before this runs.
app.commandLine.appendSwitch('ozone-platform-hint', 'auto');
app.commandLine.appendSwitch('enable-features', 'WaylandWindowDecorations');

function arg(name: string): string | undefined {
  const argv = process.argv;
  const i = argv.indexOf(`--${name}`);
  if (i >= 0 && i + 1 < argv.length) return argv[i + 1];
  const eq = argv.find((a) => a.startsWith(`--${name}=`));
  return eq ? eq.slice(name.length + 3) : undefined;
}

const port = Number(arg('port') ?? 7878);
const repoRoot = resolve(arg('root') ?? process.env.DRUMS_ROOT ?? join(app.getAppPath(), '..'));
const engineBinary = join(repoRoot, 'engine/build/drums-engine');
const screenshotArg = arg('screenshot');
const screenshotPath = screenshotArg ? resolve(process.cwd(), screenshotArg) : null;
const screenshotWaitMs = Number(arg('screenshot-wait') ?? 9) * 1000;
const [winW, winH] = (arg('size') ?? '1440x960').split('x').map(Number);
const themeArg = arg('theme');

let engine: ChildProcess | null = null;
let engineStatus: EngineProcessStatus = 'missing';
let mainWindow: BrowserWindow | null = null;

function portOpen(p: number): Promise<boolean> {
  return new Promise((done) => {
    const s = createConnection({ host: '127.0.0.1', port: p });
    s.setTimeout(500, () => {
      s.destroy();
      done(false);
    });
    s.once('connect', () => {
      s.destroy();
      done(true);
    });
    s.once('error', () => done(false));
  });
}

function setEngineStatus(status: EngineProcessStatus) {
  engineStatus = status;
  mainWindow?.webContents.send('engine:status', status);
}

async function ensureEngine() {
  if (await portOpen(port)) {
    console.log(`[drums-ui] something already listens on ${port}; connecting to it`);
    setEngineStatus('external');
    return;
  }
  if (!existsSync(engineBinary)) {
    console.log(`[drums-ui] no engine on ${port} and ${engineBinary} not built; waiting for one (npm run mock?)`);
    setEngineStatus('missing');
    return;
  }
  const args = ['--root', repoRoot];
  if (port !== 7878) args.push('--port', String(port));
  console.log(`[drums-ui] starting ${engineBinary} ${args.join(' ')}`);
  engine = spawn(engineBinary, args, { cwd: repoRoot, stdio: ['ignore', 'inherit', 'inherit'] });
  setEngineStatus('spawned');
  engine.on('error', (e) => console.error(`[drums-ui] engine: ${e.message}`));
  engine.on('exit', (code, signal) => {
    console.log(`[drums-ui] engine exited (${signal ?? code})`);
    engine = null;
    setEngineStatus('exited');
  });
}

function killEngine() {
  const e = engine;
  if (!e || e.exitCode !== null) return;
  e.kill('SIGTERM');
  setTimeout(() => {
    if (e.exitCode === null) e.kill('SIGKILL');
  }, 2000).unref();
}

function listExercises(): ExerciseEntry[] {
  const dir = join(repoRoot, 'exercises');
  if (!existsSync(dir)) return [];
  return readdirSync(dir)
    .filter((f) => f.endsWith('.json'))
    .sort()
    .map((f) => {
      const path = join(dir, f);
      const file = relative(repoRoot, path);
      try {
        const j = JSON.parse(stripJsonComments(readFileSync(path, 'utf8')));
        return { id: String(j.id ?? f), title: String(j.title ?? j.id ?? f), file, path };
      } catch {
        return { id: f, title: `${f} (invalid JSON)`, file, path, invalid: true };
      }
    });
}

function readOmarchyTheme(): OmarchyTheme | null {
  const bases = [join(homedir(), '.config/omarchy/current'), join(homedir(), '.local/state/omarchy/current')];
  for (const base of bases) {
    const file = join(base, 'theme/colors.toml');
    if (!existsSync(file)) continue;
    try {
      const colors: Record<string, string> = {};
      for (const line of readFileSync(file, 'utf8').split('\n')) {
        const m = line.match(/^\s*([A-Za-z_]+)\s*=\s*"([^"]*)"/);
        if (m) colors[m[1]] = m[2];
      }
      if (!colors.background || !colors.foreground) continue;
      let name = 'omarchy';
      try {
        name = readFileSync(join(base, 'theme.name'), 'utf8').trim() || name;
      } catch {
        /* optional */
      }
      return { name, mode: colors.mode === 'light' ? 'light' : 'dark', colors };
    } catch {
      /* try next */
    }
  }
  return null;
}

function createWindow() {
  const screenshot = screenshotPath !== null;
  mainWindow = new BrowserWindow({
    width: winW || 1440,
    height: winH || 960,
    minWidth: 960,
    minHeight: 640,
    show: false,
    title: 'Drums',
    backgroundColor: '#0e1016',
    autoHideMenuBar: true,
    webPreferences: {
      preload: join(__dirname, '../preload/index.js'),
      sandbox: true,
      contextIsolation: true,
      // Keep timers running when the window is covered.
      backgroundThrottling: false,
      // --screenshot renders offscreen (no window pops up; rAF keeps running at the frame rate).
      offscreen: screenshot,
    },
  });
  if (screenshot) mainWindow.webContents.setFrameRate(60);
  else mainWindow.once('ready-to-show', () => mainWindow?.show());
  mainWindow.on('closed', () => (mainWindow = null));

  const devUrl = process.env.ELECTRON_RENDERER_URL;
  if (devUrl) void mainWindow.loadURL(devUrl);
  else void mainWindow.loadFile(join(__dirname, '../renderer/index.html'));

  if (screenshot) setupScreenshot(mainWindow, screenshotPath!);
}

function setupScreenshot(win: BrowserWindow, file: string) {
  let done = false;
  win.webContents.on('console-message', (e) => console.log(`[renderer] ${e.message}`));
  const capture = async (reason: string) => {
    if (done) return;
    done = true;
    try {
      const image = await win.webContents.capturePage();
      mkdirSync(dirname(file), { recursive: true });
      writeFileSync(file, image.toPNG());
      const { width, height } = image.getSize();
      console.log(`[drums-ui] screenshot ${file} (${width}x${height}, ${reason})`);
    } catch (e) {
      console.error(`[drums-ui] screenshot failed: ${(e as Error).message}`);
      process.exitCode = 1;
    }
    app.quit();
  };
  ipcMain.once('screenshot:ready', () => void capture('ready'));
  setTimeout(() => void capture('timeout: no engine data'), screenshotWaitMs + 20000);
}

ipcMain.handle('app:info', (): AppInfo => ({
  repoRoot,
  wsUrl: `ws://127.0.0.1:${port}`,
  engine: engineStatus,
  engineBinary,
  screenshot: screenshotPath ? { waitMs: screenshotWaitMs } : null,
  theme: themeArg === 'dark' || themeArg === 'omarchy' ? themeArg : null,
}));
ipcMain.handle('exercises:list', () => listExercises());
ipcMain.handle('theme:omarchy', () => readOmarchyTheme());
ipcMain.handle('exercises:open-dialog', async () => {
  const opts = {
    title: 'Load exercise',
    defaultPath: join(repoRoot, 'exercises'),
    filters: [{ name: 'Exercise', extensions: ['json'] }],
    properties: ['openFile' as const],
  };
  const res = mainWindow ? await dialog.showOpenDialog(mainWindow, opts) : await dialog.showOpenDialog(opts);
  return res.canceled || res.filePaths.length === 0 ? null : res.filePaths[0];
});

app.whenReady().then(async () => {
  Menu.setApplicationMenu(null);
  await ensureEngine();
  createWindow();
});

app.on('window-all-closed', () => app.quit());
app.on('will-quit', killEngine);
process.on('exit', killEngine);
for (const sig of ['SIGINT', 'SIGTERM'] as const) process.on(sig, () => app.quit());
