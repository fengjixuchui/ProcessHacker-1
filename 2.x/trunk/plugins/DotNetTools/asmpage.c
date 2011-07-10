/*
 * Process Hacker .NET Tools - 
 *   .NET Assemblies property page
 * 
 * Copyright (C) 2011 wj32
 * 
 * This file is part of Process Hacker.
 * 
 * Process Hacker is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Process Hacker is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Process Hacker.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "dn.h"
#include "resource.h"
#include <windowsx.h>
#include <evntcons.h>
#include "clretw.h"

#define CLR_VERSION_1_0 0x1
#define CLR_VERSION_1_1 0x2
#define CLR_VERSION_2_0 0x4
#define CLR_VERSION_4_ABOVE 0x8

#define DNATNC_STRUCTURE 0
#define DNATNC_ID 1
#define DNATNC_FLAGS 2
#define DNATNC_PATH 3
#define DNATNC_NATIVEPATH 4
#define DNATNC_MAXIMUM 5

#define DNA_TYPE_CLR 1
#define DNA_TYPE_APPDOMAIN 2
#define DNA_TYPE_ASSEMBLY 3

typedef struct _DNA_NODE
{
    PH_TREENEW_NODE Node;

    struct _DNA_NODE *Parent;
    PPH_LIST Children;

    PH_STRINGREF TextCache[DNATNC_MAXIMUM];

    ULONG Type;
    BOOLEAN IsFakeClr;

    union
    {
        struct
        {
            USHORT ClrInstanceID;
            PPH_STRING DisplayName;
        } Clr;
        struct
        {
            ULONG64 AppDomainID;
            PPH_STRING DisplayName;
        } AppDomain;
        struct
        {
            ULONG64 AssemblyID;
            PPH_STRING FullyQualifiedAssemblyName;
        } Assembly;
    } u;

    PH_STRINGREF StructureText;
    PPH_STRING IdText;
    PPH_STRING FlagsText;
    PPH_STRING PathText;
    PPH_STRING NativePathText;
} DNA_NODE, *PDNA_NODE;

typedef struct _ASMPAGE_CONTEXT
{
    HWND WindowHandle;
    PPH_PROCESS_ITEM ProcessItem;
    ULONG ClrVersions;
    PDNA_NODE ClrV2Node;
    TRACEHANDLE TraceHandle;
    HWND TnHandle;
    PPH_LIST NodeList;
    PPH_LIST NodeRootList;
} ASMPAGE_CONTEXT, *PASMPAGE_CONTEXT;

typedef struct _FLAG_DEFINITION
{
    PWSTR Name;
    ULONG Flag;
} FLAG_DEFINITION, *PFLAG_DEFINITION;

typedef ULONG (__stdcall *_EnableTraceEx)(
    __in LPCGUID ProviderId,
    __in_opt LPCGUID SourceId,
    __in TRACEHANDLE TraceHandle,
    __in ULONG IsEnabled,
    __in UCHAR Level,
    __in ULONGLONG MatchAnyKeyword,
    __in ULONGLONG MatchAllKeyword,
    __in ULONG EnableProperty,
    __in_opt PEVENT_FILTER_DESCRIPTOR EnableFilterDesc
    );

INT_PTR CALLBACK DotNetAsmPageDlgProc(
    __in HWND hwndDlg,
    __in UINT uMsg,
    __in WPARAM wParam,
    __in LPARAM lParam
    );

static PH_STRINGREF DotNetLoggerName = PH_STRINGREF_INIT(L"PhDnLogger");
static GUID ClrRuntimeProviderGuid = { 0xe13c0d23, 0xccbc, 0x4e12, { 0x93, 0x1b, 0xd9, 0xcc, 0x2e, 0xee, 0x27, 0xe4 } };
static GUID ClrRundownProviderGuid = { 0xa669021c, 0xc450, 0x4609, { 0xa0, 0x35, 0x5a, 0xf5, 0x9a, 0xf4, 0xdf, 0x18 } };

static FLAG_DEFINITION AppDomainFlagsMap[] =
{
    { L"Default", 0x1 },
    { L"Executable", 0x2 },
    { L"Shared", 0x4 }
};

static FLAG_DEFINITION AssemblyFlagsMap[] =
{
    { L"DomainNeutral", 0x1 },
    { L"Dynamic", 0x2 },
    { L"Native", 0x4 },
    { L"Collectible", 0x8 }
};

static FLAG_DEFINITION ModuleFlagsMap[] =
{
    { L"DomainNeutral", 0x1 },
    { L"Native", 0x2 },
    { L"Dynamic", 0x4 },
    { L"Manifest", 0x8 }
};

static FLAG_DEFINITION StartupModeMap[] =
{
    { L"ManagedExe", 0x1 },
    { L"HostedCLR", 0x2 },
    { L"IjwDll", 0x4 },
    { L"ComActivated", 0x8 },
    { L"Other", 0x10 }
};

static FLAG_DEFINITION StartupFlagsMap[] =
{
    { L"CONCURRENT_GC", 0x1 },
    { L"LOADER_OPTIMIZATION_SINGLE_DOMAIN", 0x2 },
    { L"LOADER_OPTIMIZATION_MULTI_DOMAIN", 0x4 },
    { L"LOADER_SAFEMODE", 0x10 },
    { L"LOADER_SETPREFERENCE", 0x100 },
    { L"SERVER_GC", 0x1000 },
    { L"HOARD_GC_VM", 0x2000 },
    { L"SINGLE_VERSION_HOSTING_INTERFACE", 0x4000 },
    { L"LEGACY_IMPERSONATION", 0x10000 },
    { L"DISABLE_COMMITTHREADSTACK", 0x20000 },
    { L"ALWAYSFLOW_IMPERSONATION", 0x40000 },
    { L"TRIM_GC_COMMIT", 0x80000 },
    { L"ETW", 0x100000 },
    { L"SERVER_BUILD", 0x200000 },
    { L"ARM", 0x400000 }
};

VOID AddAsmPageToPropContext(
    __in PPH_PLUGIN_PROCESS_PROPCONTEXT PropContext
    )
{
    PhAddProcessPropPage(
        PropContext->PropContext,
        PhCreateProcessPropPageContextEx(PluginInstance->DllBase, MAKEINTRESOURCE(IDD_PROCDOTNETASM), DotNetAsmPageDlgProc, NULL)
        );
}

PPH_STRING FlagsToString(
    __in ULONG Flags,
    __in PFLAG_DEFINITION Map,
    __in ULONG SizeOfMap
    )
{
    PH_STRING_BUILDER sb;
    ULONG i;

    PhInitializeStringBuilder(&sb, 100);

    for (i = 0; i < SizeOfMap / sizeof(FLAG_DEFINITION); i++)
    {
        if (Flags & Map[i].Flag)
        {
            PhAppendStringBuilder2(&sb, Map[i].Name);
            PhAppendStringBuilder2(&sb, L", ");
        }
    }

    if (sb.String->Length != 0)
        PhRemoveStringBuilder(&sb, sb.String->Length / 2 - 2, 2);

    return PhFinalStringBuilderString(&sb);
}

PDNA_NODE AddNode(
    __inout PASMPAGE_CONTEXT Context
    )
{
    PDNA_NODE node;

    node = PhAllocate(sizeof(DNA_NODE));
    memset(node, 0, sizeof(DNA_NODE));
    PhInitializeTreeNewNode(&node->Node);

    memset(node->TextCache, 0, sizeof(PH_STRINGREF) * DNATNC_MAXIMUM);
    node->Node.TextCache = node->TextCache;
    node->Node.TextCacheSize = DNATNC_MAXIMUM;

    node->Children = PhCreateList(1);

    PhAddItemList(Context->NodeList, node);

    return node;
}

VOID DestroyNode(
    __in PDNA_NODE Node
    )
{
    PhDereferenceObject(Node->Children);

    if (Node->Type == DNA_TYPE_CLR)
    {
        if (Node->u.Clr.DisplayName) PhDereferenceObject(Node->u.Clr.DisplayName);
    }
    else if (Node->Type == DNA_TYPE_APPDOMAIN)
    {
        if (Node->u.AppDomain.DisplayName) PhDereferenceObject(Node->u.AppDomain.DisplayName);
    }
    else if (Node->Type == DNA_TYPE_ASSEMBLY)
    {
        if (Node->u.Assembly.FullyQualifiedAssemblyName) PhDereferenceObject(Node->u.Assembly.FullyQualifiedAssemblyName);
    }

    if (Node->IdText) PhDereferenceObject(Node->IdText);
    if (Node->FlagsText) PhDereferenceObject(Node->FlagsText);
    if (Node->PathText) PhDereferenceObject(Node->PathText);
    if (Node->NativePathText) PhDereferenceObject(Node->NativePathText);

    PhFree(Node);
}

PDNA_NODE AddFakeClrNode(
    __in PASMPAGE_CONTEXT Context,
    __in PWSTR DisplayName
    )
{
    PDNA_NODE node;

    node = AddNode(Context);
    node->Type = DNA_TYPE_CLR;
    node->IsFakeClr = TRUE;
    node->u.Clr.ClrInstanceID = 0;
    node->u.Clr.DisplayName = NULL;
    PhInitializeStringRef(&node->StructureText, DisplayName);

    PhAddItemList(Context->NodeRootList, node);

    return node;
}

PDNA_NODE FindClrNode(
    __in PASMPAGE_CONTEXT Context,
    __in USHORT ClrInstanceID
    )
{
    ULONG i;

    for (i = 0; i < Context->NodeRootList->Count; i++)
    {
        PDNA_NODE node = Context->NodeRootList->Items[i];

        if (!node->IsFakeClr && node->u.Clr.ClrInstanceID == ClrInstanceID)
            return node;
    }

    return NULL;
}

PDNA_NODE FindAppDomainNode(
    __in PDNA_NODE ClrNode,
    __in ULONG64 AppDomainID
    )
{
    ULONG i;

    for (i = 0; i < ClrNode->Children->Count; i++)
    {
        PDNA_NODE node = ClrNode->Children->Items[i];

        if (node->u.AppDomain.AppDomainID == AppDomainID)
            return node;
    }

    return NULL;
}

PDNA_NODE FindAssemblyNode(
    __in PDNA_NODE ClrNode,
    __in ULONG64 AssemblyID
    )
{
    ULONG i;
    ULONG j;

    for (i = 0; i < ClrNode->Children->Count; i++)
    {
        PDNA_NODE appDomainNode = ClrNode->Children->Items[i];

        for (j = 0; j < appDomainNode->Children->Count; j++)
        {
            PDNA_NODE assemblyNode = appDomainNode->Children->Items[j];

            if (assemblyNode->u.Assembly.AssemblyID == AssemblyID)
                return assemblyNode;
        }
    }

    return NULL;
}

static int __cdecl AssemblyNodeNameCompareFunction(
    __in const void *elem1,
    __in const void *elem2
    )
{
    PDNA_NODE node1 = *(PDNA_NODE *)elem1;
    PDNA_NODE node2 = *(PDNA_NODE *)elem2;

    return PhCompareStringRef(&node1->StructureText, &node2->StructureText, TRUE);
}

BOOLEAN NTAPI DotNetAsmTreeNewCallback(
    __in HWND hwnd,
    __in PH_TREENEW_MESSAGE Message,
    __in_opt PVOID Parameter1,
    __in_opt PVOID Parameter2,
    __in_opt PVOID Context
    )
{
    PASMPAGE_CONTEXT context;
    PDNA_NODE node;

    context = Context;

    switch (Message)
    {
    case TreeNewGetChildren:
        {
            PPH_TREENEW_GET_CHILDREN getChildren = Parameter1;

            node = (PDNA_NODE)getChildren->Node;

            if (!node)
            {
                getChildren->Children = (PPH_TREENEW_NODE *)context->NodeRootList->Items;
                getChildren->NumberOfChildren = context->NodeRootList->Count;
            }
            else
            {
                if (node->Type == DNA_TYPE_APPDOMAIN || node == context->ClrV2Node)
                {
                    // Sort the assemblies.
                    qsort(node->Children->Items, node->Children->Count, sizeof(PVOID), AssemblyNodeNameCompareFunction);
                }

                getChildren->Children = (PPH_TREENEW_NODE *)node->Children->Items;
                getChildren->NumberOfChildren = node->Children->Count;
            }
        }
        return TRUE;
    case TreeNewIsLeaf:
        {
            PPH_TREENEW_IS_LEAF isLeaf = Parameter1;

            node = (PDNA_NODE)isLeaf->Node;

            isLeaf->IsLeaf = node->Children->Count == 0;
        }
        return TRUE;
    case TreeNewGetCellText:
        {
            PPH_TREENEW_GET_CELL_TEXT getCellText = Parameter1;

            node = (PDNA_NODE)getCellText->Node;

            switch (getCellText->Id)
            {
            case DNATNC_STRUCTURE:
                getCellText->Text = node->StructureText;
                break;
            case DNATNC_ID:
                getCellText->Text = PhGetStringRef(node->IdText);
                break;
            case DNATNC_FLAGS:
                getCellText->Text = PhGetStringRef(node->FlagsText);
                break;
            case DNATNC_PATH:
                getCellText->Text = PhGetStringRef(node->PathText);
                break;
            case DNATNC_NATIVEPATH:
                getCellText->Text = PhGetStringRef(node->NativePathText);
                break;
            default:
                return FALSE;
            }

            getCellText->Flags = TN_CACHE;
        }
        return TRUE;
    case TreeNewGetCellTooltip:
        {
            PPH_TREENEW_GET_CELL_TOOLTIP getCellTooltip = Parameter1;

            node = (PDNA_NODE)getCellTooltip->Node;

            if (getCellTooltip->Column->Id != 0 || node->Type != DNA_TYPE_ASSEMBLY)
                return FALSE;

            if (!PhIsNullOrEmptyString(node->u.Assembly.FullyQualifiedAssemblyName))
            {
                getCellTooltip->Text = node->u.Assembly.FullyQualifiedAssemblyName->sr;
                getCellTooltip->Unfolding = FALSE;
            }
            else
            {
                return FALSE;
            }
        }
        return TRUE;
    case TreeNewKeyDown:
        {
            PPH_TREENEW_KEY_EVENT keyEvent = Parameter1;

            switch (keyEvent->VirtualKey)
            {
            case 'C':
                if (GetKeyState(VK_CONTROL) < 0)
                    SendMessage(context->WindowHandle, WM_COMMAND, ID_COPY, 0);
                break;
            }
        }
        return TRUE;
    }

    return FALSE;
}

ULONG StartDotNetTrace(
    __in PASMPAGE_CONTEXT Context,
    __out PTRACEHANDLE SessionHandle,
    __out PEVENT_TRACE_PROPERTIES *Properties
    )
{
    ULONG result;
    ULONG bufferSize;
    PEVENT_TRACE_PROPERTIES properties;
    TRACEHANDLE sessionHandle;

    bufferSize = sizeof(EVENT_TRACE_PROPERTIES) + DotNetLoggerName.Length + sizeof(WCHAR);
    properties = PhAllocate(bufferSize);
    memset(properties, 0, sizeof(EVENT_TRACE_PROPERTIES));

    properties->Wnode.BufferSize = bufferSize;
    properties->Wnode.ClientContext = 2; // System time clock resolution
    properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_USE_PAGED_MEMORY;
    properties->LogFileNameOffset = 0;
    properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

    result = StartTrace(&sessionHandle, DotNetLoggerName.Buffer, properties);

    if (result == 0)
    {
        *SessionHandle = sessionHandle;
        *Properties = properties;

        return 0;
    }
    else if (result == ERROR_ALREADY_EXISTS)
    {
        // Session already exists, so use that. Get the existing session handle.

        result = ControlTrace(0, DotNetLoggerName.Buffer, properties, EVENT_TRACE_CONTROL_QUERY);

        if (result != 0)
        {
            PhFree(properties);
            return result;
        }

        *SessionHandle = properties->Wnode.HistoricalContext;
        *Properties = properties;

        return 0;
    }
    else
    {
        PhFree(properties);

        return result;
    }
}

ULONG NTAPI DotNetBufferCallback(
    __in PEVENT_TRACE_LOGFILE Buffer
    )
{
    return TRUE;
}

VOID NTAPI DotNetEventCallback(
    __in PEVENT_RECORD EventRecord
    )
{
    PASMPAGE_CONTEXT context = EventRecord->UserContext;
    PEVENT_HEADER eventHeader = &EventRecord->EventHeader;
    PEVENT_DESCRIPTOR eventDescriptor = &eventHeader->EventDescriptor;

    if (UlongToHandle(eventHeader->ProcessId) == context->ProcessItem->ProcessId || eventDescriptor->Id == DCStartComplete_V1)
    {
        // .NET 4.0+

        switch (eventDescriptor->Id)
        {
        case RuntimeInformationDCStart:
            {
                PRuntimeInformationRundown data = EventRecord->UserData;
                PDNA_NODE node;
                PPH_STRING startupFlagsString;
                PPH_STRING startupModeString;

                // Check for duplicates.
                if (FindClrNode(context, data->ClrInstanceID))
                    break;

                node = AddNode(context);
                node->Type = DNA_TYPE_CLR;
                node->u.Clr.ClrInstanceID = data->ClrInstanceID;
                node->u.Clr.DisplayName = PhFormatString(L"CLR v%u.%u.%u.%u", data->VMMajorVersion, data->VMMinorVersion, data->VMBuildNumber, data->VMQfeNumber);
                node->StructureText = node->u.Clr.DisplayName->sr;
                node->IdText = PhFormatString(L"%u", data->ClrInstanceID);

                startupFlagsString = FlagsToString(data->StartupFlags, StartupFlagsMap, sizeof(StartupFlagsMap));
                startupModeString = FlagsToString(data->StartupMode, StartupModeMap, sizeof(StartupModeMap));

                if (startupFlagsString->Length != 0 && startupModeString->Length != 0)
                {
                    node->FlagsText = PhConcatStrings(3, startupFlagsString->Buffer, L", ", startupModeString->Buffer);
                    PhDereferenceObject(startupFlagsString);
                    PhDereferenceObject(startupModeString);
                }
                else if (startupFlagsString->Length != 0)
                {
                    node->FlagsText = startupFlagsString;
                    PhDereferenceObject(startupModeString);
                }
                else if (startupModeString->Length != 0)
                {
                    node->FlagsText = startupModeString;
                    PhDereferenceObject(startupFlagsString);
                }

                if (data->CommandLine[0])
                    node->PathText = PhCreateString(data->CommandLine);

                PhAddItemList(context->NodeRootList, node);
            }
            break;
        case AppDomainDCStart_V1:
            {
                PAppDomainLoadUnloadRundown_V1 data = EventRecord->UserData;
                SIZE_T appDomainNameLength;
                USHORT clrInstanceID;
                PDNA_NODE parentNode;
                PDNA_NODE node;

                appDomainNameLength = wcslen(data->AppDomainName) * sizeof(WCHAR);
                clrInstanceID = *(PUSHORT)((PCHAR)data + FIELD_OFFSET(AppDomainLoadUnloadRundown_V1, AppDomainName) + appDomainNameLength + sizeof(WCHAR) + sizeof(ULONG));

                // Find the CLR node to add the AppDomain node to.
                parentNode = FindClrNode(context, clrInstanceID);

                if (parentNode)
                {
                    node = AddNode(context);
                    node->Type = DNA_TYPE_APPDOMAIN;
                    node->u.AppDomain.AppDomainID = data->AppDomainID;
                    node->u.AppDomain.DisplayName = PhConcatStrings2(L"AppDomain: ", data->AppDomainName);
                    node->StructureText = node->u.AppDomain.DisplayName->sr;
                    node->IdText = PhFormatString(L"%I64u", data->AppDomainID);
                    node->FlagsText = FlagsToString(data->AppDomainFlags, AppDomainFlagsMap, sizeof(AppDomainFlagsMap));

                    PhAddItemList(parentNode->Children, node);
                }
            }
            break;
        case AssemblyDCStart_V1:
            {
                PAssemblyLoadUnloadRundown_V1 data = EventRecord->UserData;
                SIZE_T fullyQualifiedAssemblyNameLength;
                USHORT clrInstanceID;
                PDNA_NODE parentNode;
                PDNA_NODE node;
                PH_STRINGREF remainingPart;

                fullyQualifiedAssemblyNameLength = wcslen(data->FullyQualifiedAssemblyName) * sizeof(WCHAR);
                clrInstanceID = *(PUSHORT)((PCHAR)data + FIELD_OFFSET(AssemblyLoadUnloadRundown_V1, FullyQualifiedAssemblyName) + fullyQualifiedAssemblyNameLength + sizeof(WCHAR));

                // Find the AppDomain node to add the Assembly node to.

                parentNode = FindClrNode(context, clrInstanceID);

                if (parentNode)
                    parentNode = FindAppDomainNode(parentNode, data->AppDomainID);

                if (parentNode)
                {
                    node = AddNode(context);
                    node->Type = DNA_TYPE_ASSEMBLY;
                    node->u.Assembly.AssemblyID = data->AssemblyID;
                    node->u.Assembly.FullyQualifiedAssemblyName = PhCreateStringEx(data->FullyQualifiedAssemblyName, fullyQualifiedAssemblyNameLength);

                    // Display only the assembly name, not the whole fully qualified name.
                    if (!PhSplitStringRefAtChar(&node->u.Assembly.FullyQualifiedAssemblyName->sr, ',', &node->StructureText, &remainingPart))
                        node->StructureText = node->u.Assembly.FullyQualifiedAssemblyName->sr;

                    node->IdText = PhFormatString(L"%I64u", data->AssemblyID);
                    node->FlagsText = FlagsToString(data->AssemblyFlags, AssemblyFlagsMap, sizeof(AssemblyFlagsMap));

                    PhAddItemList(parentNode->Children, node);
                }
            }
            break;
        case ModuleDCStart_V1:
            {
                PModuleLoadUnloadRundown_V1 data = EventRecord->UserData;
                PWSTR moduleILPath;
                SIZE_T moduleILPathLength;
                PWSTR moduleNativePath;
                SIZE_T moduleNativePathLength;
                USHORT clrInstanceID;
                PDNA_NODE node;

                moduleILPath = data->ModuleILPath;
                moduleILPathLength = wcslen(moduleILPath) * sizeof(WCHAR);
                moduleNativePath = (PWSTR)((PCHAR)moduleILPath + moduleILPathLength + sizeof(WCHAR));
                moduleNativePathLength = wcslen(moduleNativePath) * sizeof(WCHAR);
                clrInstanceID = *(PUSHORT)((PCHAR)moduleNativePath + moduleNativePathLength + sizeof(WCHAR));

                // Find the Assembly node to set the path on.

                node = FindClrNode(context, clrInstanceID);

                if (node)
                    node = FindAssemblyNode(node, data->AssemblyID);

                if (node)
                {
                    node->PathText = PhCreateStringEx(moduleILPath, moduleILPathLength);

                    if (moduleNativePathLength != 0)
                        node->NativePathText = PhCreateStringEx(moduleNativePath, moduleNativePathLength);
                }
            }
            break;
        case DCStartComplete_V1:
            {
                CloseTrace(context->TraceHandle);
                context->TraceHandle = 0;
            }
            break;
        }

        // .NET 2.0

        if (eventDescriptor->Id == 0)
        {
            switch (eventDescriptor->Opcode)
            {
            case CLR_MODULEDCSTART_OPCODE:
                {
                    PModuleLoadUnloadRundown_V1 data = EventRecord->UserData;
                    PWSTR moduleILPath;
                    SIZE_T moduleILPathLength;
                    PWSTR moduleNativePath;
                    SIZE_T moduleNativePathLength;
                    PDNA_NODE node;
                    ULONG indexOfBackslash;
                    ULONG indexOfLastDot;

                    moduleILPath = data->ModuleILPath;
                    moduleILPathLength = wcslen(moduleILPath) * sizeof(WCHAR);
                    moduleNativePath = (PWSTR)((PCHAR)moduleILPath + moduleILPathLength + sizeof(WCHAR));
                    moduleNativePathLength = wcslen(moduleNativePath) * sizeof(WCHAR);

                    if (context->ClrV2Node)
                    {
                        node = AddNode(context);
                        node->Type = DNA_TYPE_ASSEMBLY;
                        node->FlagsText = FlagsToString(data->ModuleFlags, ModuleFlagsMap, sizeof(ModuleFlagsMap));
                        node->PathText = PhCreateStringEx(moduleILPath, moduleILPathLength);

                        if (moduleNativePathLength != 0)
                            node->NativePathText = PhCreateStringEx(moduleNativePath, moduleNativePathLength);

                        // Use the name between the last backslash and the last dot for the structure column text.
                        // (E.g. C:\...\AcmeSoft.BigLib.dll -> AcmeSoft.BigLib)

                        indexOfBackslash = PhFindLastCharInString(node->PathText, 0, '\\');
                        indexOfLastDot = PhFindLastCharInString(node->PathText, 0, '.');

                        if (indexOfBackslash != -1)
                        {
                            node->StructureText.Buffer = node->PathText->Buffer + indexOfBackslash + 1;

                            if (indexOfLastDot != -1 && indexOfLastDot > indexOfBackslash)
                            {
                                node->StructureText.Length = (USHORT)((indexOfLastDot - indexOfBackslash - 1) * sizeof(WCHAR));
                            }
                            else
                            {
                                node->StructureText.Length = node->PathText->Length - (USHORT)(indexOfBackslash * sizeof(WCHAR)) - sizeof(WCHAR);
                            }
                        }
                        else
                        {
                            node->StructureText = node->PathText->sr;
                        }

                        PhAddItemList(context->ClrV2Node->Children, node);
                    }
                }
                break;
            case CLR_METHODDC_DCSTARTCOMPLETE_OPCODE:
                {
                    CloseTrace(context->TraceHandle);
                    context->TraceHandle = 0;
                }
                break;
            }
        }
    }
}

ULONG ProcessDotNetTrace(
    __in PASMPAGE_CONTEXT Context
    )
{
    ULONG result;
    TRACEHANDLE traceHandle;
    EVENT_TRACE_LOGFILE logFile;

    memset(&logFile, 0, sizeof(EVENT_TRACE_LOGFILE));
    logFile.LoggerName = DotNetLoggerName.Buffer;
    logFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logFile.BufferCallback = DotNetBufferCallback;
    logFile.EventRecordCallback = DotNetEventCallback;
    logFile.Context = Context;

    traceHandle = OpenTrace(&logFile);

    if (traceHandle == INVALID_PROCESSTRACE_HANDLE)
        return GetLastError();

    Context->TraceHandle = traceHandle;
    result = ProcessTrace(&traceHandle, 1, NULL, NULL);

    if (Context->TraceHandle != 0)
        CloseTrace(traceHandle);

    return result;
}

ULONG UpdateDotNetTraceInfo(
    __in PASMPAGE_CONTEXT Context,
    __in BOOLEAN ClrV2
    )
{
    static _EnableTraceEx EnableTraceEx_I = NULL;

    ULONG result;
    TRACEHANDLE sessionHandle;
    PEVENT_TRACE_PROPERTIES properties;
    PGUID guidToEnable;

    if (!EnableTraceEx_I)
        EnableTraceEx_I = (_EnableTraceEx)PhGetProcAddress(L"advapi32.dll", "EnableTraceEx");
    if (!EnableTraceEx_I)
        return ERROR_NOT_SUPPORTED;

    result = StartDotNetTrace(Context, &sessionHandle, &properties);

    if (result != 0)
        return result;

    if (!ClrV2)
        guidToEnable = &ClrRundownProviderGuid;
    else
        guidToEnable = &ClrRuntimeProviderGuid;

    EnableTraceEx_I(
        guidToEnable,
        NULL,
        sessionHandle,
        1,
        TRACE_LEVEL_INFORMATION,
        CLR_LOADER_KEYWORD | CLR_STARTENUMERATION_KEYWORD,
        0,
        0,
        NULL
        );

    result = ProcessDotNetTrace(Context);

    ControlTrace(sessionHandle, NULL, properties, EVENT_TRACE_CONTROL_STOP);
    PhFree(properties);

    return result;
}

BOOLEAN NTAPI DotNetVersionsEnumModulesCallback(
    __in PPH_MODULE_INFO Module,
    __in_opt PVOID Context
    )
{
    if (
        PhEqualString2(Module->Name, L"clr.dll", TRUE) ||
        PhEqualString2(Module->Name, L"mscorwks.dll", TRUE) ||
        PhEqualString2(Module->Name, L"mscorsvr.dll", TRUE)
        )
    {
        static PH_STRINGREF frameworkString = PH_STRINGREF_INIT(L"Microsoft.NET\\Framework\\");
        PH_STRINGREF firstPart;
        PH_STRINGREF secondPart;

        if (PhSplitStringRefAtString(&Module->FileName->sr, &frameworkString, TRUE, &firstPart, &secondPart))
        {
            if (secondPart.Length >= 4 * sizeof(WCHAR)) // vx.x
            {
                if (secondPart.Buffer[1] == '1')
                {
                    if (secondPart.Buffer[3] == '0')
                        *(PULONG)Context |= CLR_VERSION_1_0;
                    else if (secondPart.Buffer[3] == '1')
                        *(PULONG)Context |= CLR_VERSION_1_1;
                }
                else if (secondPart.Buffer[1] == '2')
                {
                    *(PULONG)Context |= CLR_VERSION_2_0;
                }
                else if (secondPart.Buffer[1] >= '4' && secondPart.Buffer[1] <= '9')
                {
                    *(PULONG)Context |= CLR_VERSION_4_ABOVE;
                }
            }
        }
    }

    return TRUE;
}

ULONG GetProcessDotNetVersions(
    __in HANDLE ProcessId
    )
{
    ULONG versions;

    versions = 0;
    PhEnumGenericModules(ProcessId, NULL, 0, DotNetVersionsEnumModulesCallback, &versions);

    return versions;
}

INT_PTR CALLBACK DotNetAsmPageDlgProc(
    __in HWND hwndDlg,
    __in UINT uMsg,
    __in WPARAM wParam,
    __in LPARAM lParam
    )
{
    LPPROPSHEETPAGE propSheetPage;
    PPH_PROCESS_PROPPAGECONTEXT propPageContext;
    PPH_PROCESS_ITEM processItem;
    PASMPAGE_CONTEXT context;

    if (PhPropPageDlgProcHeader(hwndDlg, uMsg, lParam, &propSheetPage, &propPageContext, &processItem))
    {
        context = propPageContext->Context;
    }
    else
    {
        return FALSE;
    }

    switch (uMsg)
    {
    case WM_INITDIALOG:
        {
            ULONG result = 0;
            HWND tnHandle;

            context = PhAllocate(sizeof(ASMPAGE_CONTEXT));
            memset(context, 0, sizeof(ASMPAGE_CONTEXT));
            propPageContext->Context = context;
            context->WindowHandle = hwndDlg;
            context->ProcessItem = processItem;

            context->ClrVersions = GetProcessDotNetVersions(processItem->ProcessId);

            context->NodeList = PhCreateList(64);
            context->NodeRootList = PhCreateList(2);

            tnHandle = GetDlgItem(hwndDlg, IDC_LIST);
            context->TnHandle = tnHandle;

            TreeNew_SetRedraw(tnHandle, FALSE);

            PhSetWindowStyle(tnHandle, WS_BORDER, WS_BORDER);
            TreeNew_SetCallback(tnHandle, DotNetAsmTreeNewCallback, context);
            TreeNew_SetExtendedFlags(tnHandle, TN_FLAG_ITEM_DRAG_SELECT, TN_FLAG_ITEM_DRAG_SELECT);
            PhSetControlTheme(tnHandle, L"explorer");
            SendMessage(TreeNew_GetTooltips(tnHandle), TTM_SETMAXTIPWIDTH, 0, MAXSHORT);
            PhAddTreeNewColumn(tnHandle, DNATNC_STRUCTURE, TRUE, L"Structure", 240, PH_ALIGN_LEFT, -2, 0);
            PhAddTreeNewColumn(tnHandle, DNATNC_ID, TRUE, L"ID", 50, PH_ALIGN_RIGHT, 0, DT_RIGHT);
            PhAddTreeNewColumn(tnHandle, DNATNC_FLAGS, TRUE, L"Flags", 120, PH_ALIGN_LEFT, 1, 0);
            PhAddTreeNewColumn(tnHandle, DNATNC_PATH, TRUE, L"Path", 600, PH_ALIGN_LEFT, 2, 0); // don't use path ellipsis - the user already has the base file name
            PhAddTreeNewColumn(tnHandle, DNATNC_NATIVEPATH, TRUE, L"Native Image Path", 600, PH_ALIGN_LEFT, 3, 0);

            SetCursor(LoadCursor(NULL, IDC_WAIT));

            if (context->ClrVersions & CLR_VERSION_1_0)
            {
                AddFakeClrNode(context, L"CLR v1.0.3705"); // what PE displays
            }

            if (context->ClrVersions & CLR_VERSION_1_1)
            {
                AddFakeClrNode(context, L"CLR v1.1.4322");
            }

            if (context->ClrVersions & CLR_VERSION_2_0)
            {
                context->ClrV2Node = AddFakeClrNode(context, L"CLR v2.0.50727");
                result = UpdateDotNetTraceInfo(context, TRUE);
            }

            if (context->ClrVersions & CLR_VERSION_4_ABOVE)
            {
                result = UpdateDotNetTraceInfo(context, FALSE);
            }

            TreeNew_NodesStructured(tnHandle);

            TreeNew_SetRedraw(tnHandle, TRUE);
            SetCursor(LoadCursor(NULL, IDC_ARROW));

            if (result != 0)
            {
                ShowWindow(tnHandle, SW_HIDE);
                ShowWindow(GetDlgItem(hwndDlg, IDC_ERROR), SW_SHOW);

                if (result == ERROR_ACCESS_DENIED)
                {
                    SetDlgItemText(hwndDlg, IDC_ERROR, L"Unable to start the event tracing session. Make sure Process Hacker is running with administrative privileges.");
                }
                else
                {
                    SetDlgItemText(hwndDlg, IDC_ERROR,
                        PhaConcatStrings2(L"Unable to start the event tracing session: %s", PhGetStringOrDefault(PhGetWin32Message(result), L"Unknown error"))->Buffer);
                }
            }
        }
        break;
    case WM_DESTROY:
        {
            ULONG i;

            for (i = 0; i < context->NodeList->Count; i++)
                DestroyNode(context->NodeList->Items[i]);

            PhDereferenceObject(context->NodeList);
            PhDereferenceObject(context->NodeRootList);
            PhFree(context);

            PhPropPageDlgProcDestroy(hwndDlg);
        }
        break;
    case WM_SHOWWINDOW:
        {
            PPH_LAYOUT_ITEM dialogItem;

            if (dialogItem = PhBeginPropPageLayout(hwndDlg, propPageContext))
            {
                PhAddPropPageLayoutItem(hwndDlg, GetDlgItem(hwndDlg, IDC_LIST), dialogItem, PH_ANCHOR_ALL);
                PhAddPropPageLayoutItem(hwndDlg, GetDlgItem(hwndDlg, IDC_ERROR), dialogItem, PH_ANCHOR_LEFT | PH_ANCHOR_TOP | PH_ANCHOR_RIGHT | PH_LAYOUT_FORCE_INVALIDATE);
                PhEndPropPageLayout(hwndDlg, propPageContext);
            }
        }
        break;
    case WM_COMMAND:
        {
            switch (LOWORD(wParam))
            {
            case ID_COPY:
                {
                    PPH_FULL_STRING text;

                    text = PhGetTreeNewText(context->TnHandle, DNATNC_MAXIMUM);
                    PhSetClipboardStringEx(context->TnHandle, text->Buffer, text->Length);
                    PhDereferenceObject(text);
                }
                break;
            }
        }
        break;
    }

    return FALSE;
}