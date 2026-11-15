import { contextBridge, ipcRenderer } from 'electron';
import type { DrumsApi, EngineProcessStatus } from './api';

const api: DrumsApi = {
  appInfo: () => ipcRenderer.invoke('app:info'),
  listExercises: () => ipcRenderer.invoke('exercises:list'),
  openExerciseDialog: () => ipcRenderer.invoke('exercises:open-dialog'),
  omarchyTheme: () => ipcRenderer.invoke('theme:omarchy'),
  screenshotReady: () => ipcRenderer.send('screenshot:ready'),
  onEngineStatus: (cb) => {
    const listener = (_e: unknown, status: EngineProcessStatus) => cb(status);
    ipcRenderer.on('engine:status', listener);
    return () => ipcRenderer.removeListener('engine:status', listener);
  },
};

contextBridge.exposeInMainWorld('drums', api);
