﻿/*
 * Process Hacker
 * 
 * Copyright (C) 2008 wj32
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Windows.Forms;

namespace ProcessHacker
{
    public struct ProcessItem
    {
        public int PID;
        public Process Process;

        public Icon Icon;
        public string CmdLine;
        public string MemoryUsage;
        public string Name;
        public string Username;
        public string UsernameWithDomain;

        public bool IsBeingDebugged;
        public ulong LastTime;
        public string CPUUsage;
    }

    public class ProcessProvider : Provider<int, ProcessItem>
    {
        private Dictionary<int, Win32.WtsProcess> _tsProcesses;
        private ulong _lastSysTime;

        public ProcessProvider()
            : base()
        {      
            this.ProviderUpdate += new ProviderUpdateOnce(UpdateOnce);

            ulong[] systemTimes = Win32.GetSystemTimes();

            _lastSysTime = systemTimes[1] / 10000 + systemTimes[2] / 10000;
        }

        private void UpdateOnce()
        {
            Process[] processes = Process.GetProcesses();
            Dictionary<int, Win32.WtsProcess> tsProcesses = new Dictionary<int, Win32.WtsProcess>();
            List<int> pids = new List<int>();
            Dictionary<int, ProcessItem> newdictionary = new Dictionary<int, ProcessItem>();

            ulong[] systemTimes = Win32.GetSystemTimes();
            ulong thisSysTime = systemTimes[1] / 10000 + systemTimes[2] / 10000;
            ulong sysTime = thisSysTime - _lastSysTime;

            _lastSysTime = thisSysTime;

            foreach (Win32.WtsProcess process in Win32.TSEnumProcesses())
                tsProcesses.Add(process.Info.ProcessID, process);

            _tsProcesses = tsProcesses;

            foreach (int key in Dictionary.Keys)
                newdictionary.Add(key, Dictionary[key]);

            foreach (Process p in processes)
                pids.Add(p.Id);

            // look for dead processes
            foreach (int pid in Dictionary.Keys)
            {
                if (!pids.Contains(pid))
                {                 
                    this.CallDictionaryRemoved(this.Dictionary[pid]);
                    newdictionary.Remove(pid);
                }
            }

            // look for new processes
            foreach (Process p in processes)
            {
                if (!Dictionary.ContainsKey(p.Id))
                {
                    ProcessItem item = new ProcessItem();

                    item.PID = p.Id;
                    item.Process = p;

                    try
                    {
                        item.Icon = (Icon)Win32.GetProcessIcon(p).Clone();
                    }
                    catch
                    { }

                    try
                    {
                        if (p.Id == 0)
                            item.Name = "System Idle Process";
                        else
                            item.Name = p.MainModule.ModuleName;
                    }
                    catch
                    {
                        item.Name = Win32.GetNameFromPID(p.Id);

                        if (item.Name == "(error)" || item.Name == "(unknown)")
                        {
                            try
                            {
                                item.Name = "(" + p.ProcessName + ")";
                            }
                            catch
                            {
                                item.Name = "(unknown)";
                            }
                        }
                    }

                    try
                    {
                        item.MemoryUsage = Misc.GetNiceSizeName(p.PrivateMemorySize64);
                    }
                    catch
                    { }

                    try
                    {
                        using (Win32.ProcessHandle phandle =
                            new Win32.ProcessHandle(p.Id, Win32.PROCESS_RIGHTS.PROCESS_QUERY_INFORMATION))
                        {
                            try
                            {
                                ulong[] times = Win32.GetProcessTimes(phandle);

                                item.LastTime = times[2] / 10000 + times[3] / 10000;
                            }
                            catch
                            { }

                            try
                            {
                                item.IsBeingDebugged = Win32.IsBeingDebugged(phandle.Handle);
                            }
                            catch
                            { }

                            try
                            {
                                item.Username = Win32.GetProcessUsername(phandle.Handle,
                                    Properties.Settings.Default.ShowAccountDomains);
                                item.UsernameWithDomain = Win32.GetProcessUsername(phandle.Handle,
                                    true);
                            }
                            catch
                            { }
                        }
                    }
                    catch
                    {
                        if (item.Username == null)
                        {
                            try
                            {
                                item.Username = Properties.Settings.Default.ShowAccountDomains ?
                                    tsProcesses[p.Id].UsernameWithDomain : tsProcesses[p.Id].Username;
                                item.UsernameWithDomain = tsProcesses[p.Id].UsernameWithDomain;
                            }
                            catch
                            { }
                        }
                    }

                    if (p.Id == 0)
                        item.LastTime = systemTimes[0] / 10000;

                    try
                    {
                        using (Win32.ProcessHandle phandle =
                            new Win32.ProcessHandle(p.Id,
                                Win32.PROCESS_RIGHTS.PROCESS_QUERY_INFORMATION | Win32.PROCESS_RIGHTS.PROCESS_VM_READ))
                            item.CmdLine = Win32.GetProcessCmdLine(phandle);
                    }
                    catch
                    { }

                    newdictionary.Add(p.Id, item);
                    this.CallDictionaryAdded(item);
                }
                // look for modified processes
                else
                {
                    ProcessItem item = Dictionary[p.Id];
                    ProcessItem newitem = new ProcessItem();

                    newitem.Icon = item.Icon;
                    newitem.Name = item.Name;
                    newitem.PID = item.PID;
                    newitem.Process = item.Process;

                    try
                    {
                        newitem.MemoryUsage = Misc.GetNiceSizeName(p.PrivateMemorySize64);
                    }
                    catch
                    { }

                    try
                    {
                        using (Win32.ProcessHandle phandle =
                            new Win32.ProcessHandle(p.Id, Win32.PROCESS_RIGHTS.PROCESS_QUERY_INFORMATION))
                        {
                            try
                            {
                                ulong[] times = Win32.GetProcessTimes(phandle);

                                newitem.LastTime = times[2] / 10000 + times[3] / 10000;
                                newitem.CPUUsage = ((double)(newitem.LastTime - item.LastTime) * 100 / sysTime).ToString("F2");
                            }
                            catch
                            { }

                            try
                            {
                                 newitem.IsBeingDebugged = Win32.IsBeingDebugged(phandle.Handle);
                            }
                            catch
                            { }

                            try
                            {
                                newitem.Username = Win32.GetProcessUsername(phandle.Handle,
                                    Properties.Settings.Default.ShowAccountDomains);
                                newitem.UsernameWithDomain = Win32.GetProcessUsername(phandle.Handle,
                                    true);
                            }
                            catch
                            { }
                        }
                    }
                    catch
                    {
                        if (newitem.Username == null)
                        {
                            try
                            {
                                newitem.Username = Properties.Settings.Default.ShowAccountDomains ?
                                    tsProcesses[p.Id].UsernameWithDomain : tsProcesses[p.Id].Username;
                                newitem.UsernameWithDomain = tsProcesses[p.Id].UsernameWithDomain;
                            }
                            catch
                            { }
                        }
                    }

                    if (p.Id == 0)
                    {
                        newitem.LastTime = systemTimes[0] / 10000;
                        newitem.CPUUsage = ((double)(newitem.LastTime - item.LastTime) * 100 / sysTime).ToString("F2");
                    }

                    if (newitem.MemoryUsage != item.MemoryUsage ||
                        newitem.CPUUsage != item.CPUUsage || 
                        newitem.Username != item.Username || 
                        newitem.IsBeingDebugged != item.IsBeingDebugged)
                    {
                        newdictionary[p.Id] = newitem;
                        this.CallDictionaryModified(item, newitem);
                    }
                }
            }

            Dictionary = newdictionary;
        }

        public Dictionary<int, Win32.WtsProcess> TSProcesses
        {
            get { return _tsProcesses; }
        }
    }
}
