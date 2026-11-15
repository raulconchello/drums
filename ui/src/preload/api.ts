// API exposed by the preload script as window.drums (shared by preload, main and renderer).

export interface ExerciseEntry {
  id: string;
  title: string;
  /** Path relative to the repo root, as sent to the engine with loadExercise. */
  file: string;
  /** Absolute path. */
  path: string;
  invalid?: boolean;
}

export interface OmarchyTheme {
  name: string;
  mode: 'light' | 'dark';
  colors: Record<string, string>;
}

export type EngineProcessStatus = 'external' | 'spawned' | 'missing' | 'exited';

export interface AppInfo {
  repoRoot: string;
  wsUrl: string;
  engine: EngineProcessStatus;
  engineBinary: string;
  /** Set when started with --screenshot: how long to collect data before capturing. */
  screenshot: { waitMs: number } | null;
  /** --theme dark|omarchy from the command line (overrides the saved choice). */
  theme: 'dark' | 'omarchy' | null;
}

export interface DrumsApi {
  appInfo(): Promise<AppInfo>;
  listExercises(): Promise<ExerciseEntry[]>;
  openExerciseDialog(): Promise<string | null>;
  omarchyTheme(): Promise<OmarchyTheme | null>;
  screenshotReady(): void;
  onEngineStatus(cb: (status: EngineProcessStatus) => void): () => void;
}

declare global {
  interface Window {
    drums?: DrumsApi;
  }
}
