/* Direct Play 2,3,4 Implementation
 *
 * Copyright 1998,1999,2000,2001 - Peter Hunnisett
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define COBJMACROS

#include <stdarg.h>
#include <string.h>

#include "windef.h"
#include "winerror.h"
#include "winbase.h"
#include "winnt.h"
#include "winreg.h"
#include "winnls.h"
#include "wine/debug.h"

#include "dplayx_global.h"
#include "name_server.h"
#include "dplayx_queue.h"
#include "wine/dplaysp.h"
#include "dplay_global.h"

WINE_DEFAULT_DEBUG_CHANNEL(dplay);

/* FIXME: Should this be externed? */
extern HRESULT DPL_CreateCompoundAddress
( LPCDPCOMPOUNDADDRESSELEMENT lpElements, DWORD dwElementCount,
  LPVOID lpAddress, LPDWORD lpdwAddressSize, BOOL bAnsiInterface );


/* Local function prototypes */
static lpPlayerList DP_FindPlayer( IDirectPlayImpl *This, DPID dpid );
static BOOL DP_CopyDPNAMEStruct( LPDPNAME lpDst, const DPNAME *lpSrc, BOOL bAnsi );
static void DP_SetGroupData( lpGroupData lpGData, DWORD dwFlags,
                             LPVOID lpData, DWORD dwDataSize );
static void DP_DeleteDPNameStruct( LPDPNAME lpDPName );
static BOOL CALLBACK cbDeletePlayerFromAllGroups( DPID dpId,
                                                  DWORD dwPlayerType,
                                                  LPCDPNAME lpName,
                                                  DWORD dwFlags,
                                                  LPVOID lpContext );
static lpGroupData DP_FindAnyGroup( IDirectPlayImpl *This, DPID dpid );

/* Helper methods for player/group interfaces */
static HRESULT DP_SetSessionDesc( IDirectPlayImpl *This, const DPSESSIONDESC2 *lpSessDesc,
        DWORD dwFlags, BOOL bInitial, BOOL bAnsi );
static HRESULT DP_SP_SendEx( IDirectPlayImpl *This, DWORD dwFlags, void *lpData, DWORD dwDataSize,
        DWORD dwPriority, DWORD dwTimeout, void *lpContext, DWORD *lpdwMsgID );
static BOOL DP_BuildSPCompoundAddr( LPGUID lpcSpGuid, LPVOID* lplpAddrBuf,
                                    LPDWORD lpdwBufSize );

static DPID DP_GetRemoteNextObjectId(void);

static DWORD DP_CalcSessionDescSize( LPCDPSESSIONDESC2 lpSessDesc, BOOL bAnsi );
static void DP_CopySessionDesc( LPDPSESSIONDESC2 destSessionDesc,
                                LPCDPSESSIONDESC2 srcSessDesc, BOOL bAnsi );


#define DPID_NOPARENT_GROUP 0 /* Magic number to indicate no parent of group */
#define DPID_SYSTEM_GROUP DPID_NOPARENT_GROUP /* If system group is supported
                                                 we don't have to change much */
#define DPID_NAME_SERVER 0x19a9d65b  /* Don't ask me why */

/* Strip out dwFlag values which cannot be sent in the CREATEGROUP msg */
#define DPMSG_CREATEGROUP_DWFLAGS(x) ( (x) & DPGROUP_HIDDEN )

/* Strip out all dwFlags values for CREATEPLAYER msg */
#define DPMSG_CREATEPLAYER_DWFLAGS(x) 0

static LONG kludgePlayerGroupId = 1000;


static inline IDirectPlayImpl *impl_from_IDirectPlay( IDirectPlay *iface )
{
    return CONTAINING_RECORD( iface, IDirectPlayImpl, IDirectPlay_iface );
}

static inline IDirectPlayImpl *impl_from_IDirectPlay2( IDirectPlay2 *iface )
{
    return CONTAINING_RECORD( iface, IDirectPlayImpl, IDirectPlay2_iface );
}

static inline IDirectPlayImpl *impl_from_IDirectPlay2A( IDirectPlay2A *iface )
{
    return CONTAINING_RECORD( iface, IDirectPlayImpl, IDirectPlay2A_iface );
}

static inline IDirectPlayImpl *impl_from_IDirectPlay3A( IDirectPlay3A *iface )
{
    return CONTAINING_RECORD( iface, IDirectPlayImpl, IDirectPlay3A_iface );
}

static inline IDirectPlayImpl *impl_from_IDirectPlay3( IDirectPlay3 *iface )
{
    return CONTAINING_RECORD( iface, IDirectPlayImpl, IDirectPlay3_iface );
}

static inline IDirectPlayImpl *impl_from_IDirectPlay4A( IDirectPlay4A *iface )
{
    return CONTAINING_RECORD( iface, IDirectPlayImpl, IDirectPlay4A_iface );
}

static inline IDirectPlayImpl *impl_from_IDirectPlay4( IDirectPlay4 *iface )
{
    return CONTAINING_RECORD( iface, IDirectPlayImpl, IDirectPlay4_iface );
}

static BOOL DP_CreateDirectPlay2( LPVOID lpDP )
{
  IDirectPlayImpl *This = lpDP;

  This->dp2 = calloc( 1, sizeof( *(This->dp2) ) );
  if ( This->dp2 == NULL )
  {
    return FALSE;
  }

  This->dp2->bConnectionOpen = FALSE;

  This->dp2->hEnumSessionThread = INVALID_HANDLE_VALUE;
  This->dp2->dwEnumSessionLock = 0;

  This->dp2->bHostInterface = FALSE;

  DPQ_INIT(This->dp2->receiveMsgs);
  DPQ_INIT(This->dp2->sendMsgs);
  DPQ_INIT(This->dp2->repliesExpected);

  if( !NS_InitializeSessionCache( &This->dp2->lpNameServerData ) )
  {
    /* FIXME: Memory leak */
    return FALSE;
  }

  /* Provide an initial session desc with nothing in it */
  This->dp2->lpSessionDesc = calloc( 1, sizeof( *This->dp2->lpSessionDesc ) );
  if( This->dp2->lpSessionDesc == NULL )
  {
    /* FIXME: Memory leak */
    return FALSE;
  }
  This->dp2->lpSessionDesc->dwSize = sizeof( *This->dp2->lpSessionDesc );

  /* We are emulating a dp 6 implementation */
  This->dp2->spData.dwSPVersion = DPSP_MAJORVERSION;

  This->dp2->spData.lpCB = calloc( 1, sizeof( *This->dp2->spData.lpCB ) );
  This->dp2->spData.lpCB->dwSize = sizeof( *This->dp2->spData.lpCB );
  This->dp2->spData.lpCB->dwVersion = DPSP_MAJORVERSION;

  /* This is the pointer to the service provider */
  if ( FAILED( dplaysp_create( &IID_IDirectPlaySP, (void**)&This->dp2->spData.lpISP, This ) ) )
  {
    /* FIXME: Memory leak */
    return FALSE;
  }

  /* Setup lobby provider information */
  This->dp2->dplspData.dwSPVersion = DPSP_MAJORVERSION;
  This->dp2->dplspData.lpCB = calloc( 1, sizeof( *This->dp2->dplspData.lpCB ) );
  This->dp2->dplspData.lpCB->dwSize = sizeof(  *This->dp2->dplspData.lpCB );

  if( FAILED( dplobbysp_create( &IID_IDPLobbySP, (void**)&This->dp2->dplspData.lpISP, This ) )
    )
  {
    /* FIXME: Memory leak */
    return FALSE;
  }

  return TRUE;
}

/* Definition of the global function in dplayx_queue.h. #
 * FIXME: Would it be better to have a dplayx_queue.c for this function? */
DPQ_DECL_DELETECB( cbDeleteElemFromHeap, LPVOID )
{
  free( elem );
}

static BOOL DP_DestroyDirectPlay2( LPVOID lpDP )
{
  IDirectPlayImpl *This = lpDP;

  if( This->dp2->hEnumSessionThread != INVALID_HANDLE_VALUE )
  {
    TerminateThread( This->dp2->hEnumSessionThread, 0 );
    CloseHandle( This->dp2->hEnumSessionThread );
  }

  /* Finish with the SP - have it shutdown */
  if( This->dp2->spData.lpCB->ShutdownEx )
  {
    DPSP_SHUTDOWNDATA data;

    TRACE( "Calling SP ShutdownEx\n" );

    data.lpISP = This->dp2->spData.lpISP;

    (*This->dp2->spData.lpCB->ShutdownEx)( &data );
  }
  else if (This->dp2->spData.lpCB->Shutdown ) /* obsolete interface */
  {
    TRACE( "Calling obsolete SP Shutdown\n" );
    (*This->dp2->spData.lpCB->Shutdown)();
  }

  /* Unload the SP (if it exists) */
  if( This->dp2->hServiceProvider != 0 )
  {
    FreeLibrary( This->dp2->hServiceProvider );
  }

  /* Unload the Lobby Provider (if it exists) */
  if( This->dp2->hDPLobbyProvider != 0 )
  {
    FreeLibrary( This->dp2->hDPLobbyProvider );
  }

  /* FIXME: Need to delete receive and send msgs queue contents */

  NS_DeleteSessionCache( This->dp2->lpNameServerData );

  free( This->dp2->dplspData.lpCB);
  free( This->dp2->lpSessionDesc );

  IDirectPlaySP_Release( This->dp2->spData.lpISP );

  /* Delete the contents */
  free( This->dp2 );

  return TRUE;
}

static void dplay_destroy(IDirectPlayImpl *obj)
{
     DP_DestroyDirectPlay2( obj );
     obj->lock.DebugInfo->Spare[0] = 0;
     DeleteCriticalSection( &obj->lock );
     free( obj );
}

static inline DPID DP_NextObjectId(void)
{
  return (DPID)InterlockedIncrement( &kludgePlayerGroupId );
}

/* *lplpReply will be non NULL iff there is something to reply */
HRESULT DP_HandleMessage( IDirectPlayImpl *This, const void *lpcMessageBody,
        DWORD dwMessageBodySize, const void *lpcMessageHeader, WORD wCommandId, WORD wVersion,
        void **lplpReply, DWORD *lpdwMsgSize )
{
  TRACE( "(%p)->(%p,0x%08lx,%p,%u,%u)\n",
         This, lpcMessageBody, dwMessageBodySize, lpcMessageHeader, wCommandId,
         wVersion );

  switch( wCommandId )
  {
    /* Name server needs to handle this request */
    case DPMSGCMD_ENUMSESSIONSREQUEST:
      /* Reply expected */
      NS_ReplyToEnumSessionsRequest( lpcMessageBody, lplpReply, lpdwMsgSize, This );
      break;

    /* Name server needs to handle this request */
    case DPMSGCMD_ENUMSESSIONSREPLY:
      /* No reply expected */
      NS_AddRemoteComputerAsNameServer( lpcMessageHeader,
                                        This->dp2->spData.dwSPHeaderSize,
                                        lpcMessageBody,
                                        This->dp2->lpNameServerData );
      break;

    case DPMSGCMD_REQUESTNEWPLAYERID:
    {
      LPCDPMSG_REQUESTNEWPLAYERID lpcMsg = lpcMessageBody;

      LPDPMSG_NEWPLAYERIDREPLY lpReply;

      *lpdwMsgSize = This->dp2->spData.dwSPHeaderSize + sizeof( *lpReply );

      *lplpReply = calloc( 1, *lpdwMsgSize );

      FIXME( "Ignoring dwFlags 0x%08lx in request msg\n",
             lpcMsg->dwFlags );

      /* Setup the reply */
      lpReply = (LPDPMSG_NEWPLAYERIDREPLY)( (BYTE*)(*lplpReply) +
                                            This->dp2->spData.dwSPHeaderSize );

      lpReply->envelope.dwMagic    = DPMSGMAGIC_DPLAYMSG;
      lpReply->envelope.wCommandId = DPMSGCMD_NEWPLAYERIDREPLY;
      lpReply->envelope.wVersion   = DPMSGVER_DP6;

      lpReply->dpidNewPlayerId = DP_NextObjectId();

      TRACE( "Allocating new playerid 0x%08lx from remote request\n",
             lpReply->dpidNewPlayerId );
      break;
    }

    case DPMSGCMD_GETNAMETABLEREPLY:
    case DPMSGCMD_NEWPLAYERIDREPLY:
      DP_MSG_ReplyReceived( This, wCommandId, lpcMessageBody, dwMessageBodySize );
      break;

    case DPMSGCMD_JUSTENVELOPE:
      TRACE( "GOT THE SELF MESSAGE: %p -> 0x%08lx\n", lpcMessageHeader, ((const DWORD *)lpcMessageHeader)[1] );
      NS_SetLocalAddr( This->dp2->lpNameServerData, lpcMessageHeader, 20 );
      DP_MSG_ReplyReceived( This, wCommandId, lpcMessageBody, dwMessageBodySize );

    case DPMSGCMD_FORWARDADDPLAYER:
      TRACE( "Sending message to self to get my addr\n" );
      DP_MSG_ToSelf( This, 1 ); /* This is a hack right now */
      break;

    case DPMSGCMD_FORWARDADDPLAYERNACK:
      DP_MSG_ErrorReceived( This, wCommandId, lpcMessageBody, dwMessageBodySize );
      break;

    default:
      FIXME( "Unknown wCommandId %u. Ignoring message\n", wCommandId );
      DebugBreak();
      break;
  }

  /* FIXME: There is code in dplaysp.c to handle dplay commands. Move to here. */

  return DP_OK;
}


static HRESULT WINAPI IDirectPlayImpl_QueryInterface( IDirectPlay *iface, REFIID riid, void **ppv )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    return IDirectPlayX_QueryInterface( &This->IDirectPlay4_iface, riid, ppv );
}

static ULONG WINAPI IDirectPlayImpl_AddRef( IDirectPlay *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    ULONG ref = InterlockedIncrement( &This->ref );

    TRACE( "(%p) ref=%ld\n", This, ref );

    if ( ref == 1 )
        InterlockedIncrement( &This->numIfaces );

    return ref;
}

static ULONG WINAPI IDirectPlayImpl_Release( IDirectPlay *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    ULONG ref = InterlockedDecrement( &This->ref );

    TRACE( "(%p) ref=%ld\n", This, ref );

    if ( !ref && !InterlockedDecrement( &This->numIfaces ) )
        dplay_destroy( This );

    return ref;
}

static HRESULT WINAPI IDirectPlayImpl_AddPlayerToGroup( IDirectPlay *iface, DPID group,
        DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    FIXME( "(%p)->(0x%08lx,0x%08lx): stub\n", This, group, player );
    return E_NOTIMPL;
}

static HRESULT WINAPI IDirectPlayImpl_Close( IDirectPlay *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    FIXME( "(%p): stub\n", This );
    return E_NOTIMPL;
}

static HRESULT WINAPI IDirectPlayImpl_CreatePlayer( IDirectPlay *iface, DPID *player,
        LPSTR name, LPSTR fullname, HANDLE *event )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    FIXME( "(%p)->(%p,%s,%s,%p): stub\n", This, player, debugstr_a( name ), debugstr_a( fullname ),
            event );
    return E_NOTIMPL;
}

static HRESULT WINAPI IDirectPlayImpl_CreateGroup( IDirectPlay *iface, DPID *group, LPSTR name,
        LPSTR fullname )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    FIXME( "(%p)->(%p,%s,%s): stub\n", This, group, debugstr_a( name ), debugstr_a( fullname ) );
    return E_NOTIMPL;
}

static HRESULT WINAPI IDirectPlayImpl_DeletePlayerFromGroup( IDirectPlay *iface, DPID group,
        DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    FIXME( "(%p)->(0x%08lx,0x%08lx): stub\n", This, group, player );
    return E_NOTIMPL;
}

static HRESULT WINAPI IDirectPlayImpl_DestroyPlayer( IDirectPlay *iface, DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    FIXME( "(%p)->(0x%08lx): stub\n", This, player );
    return E_NOTIMPL;
}

static HRESULT WINAPI IDirectPlayImpl_DestroyGroup( IDirectPlay *iface, DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    FIXME( "(%p)->(0x%08lx): stub\n", This, group );
    return E_NOTIMPL;
}

static HRESULT WINAPI IDirectPlayImpl_EnableNewPlayers( IDirectPlay *iface, BOOL enable )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    FIXME( "(%p)->(%d): stub\n", This, enable );
    return E_NOTIMPL;
}

static HRESULT WINAPI IDirectPlayImpl_EnumGroupPlayers( IDirectPlay *iface, DPID group,
        LPDPENUMPLAYERSCALLBACK enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    FIXME( "(%p)->(0x%08lx,%p,%p,0x%08lx): stub\n", This, group, enumplayercb, context, flags );
    return E_NOTIMPL;
}

static HRESULT WINAPI IDirectPlayImpl_EnumGroups( IDirectPlay *iface, DWORD session,
        LPDPENUMPLAYERSCALLBACK enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    FIXME( "(%p)->(0x%08lx,%p,%p,0x%08lx): stub\n", This, session, enumplayercb, context, flags );
    return E_NOTIMPL;
}

static HRESULT WINAPI IDirectPlayImpl_EnumPlayers( IDirectPlay *iface, DWORD session,
        LPDPENUMPLAYERSCALLBACK enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    FIXME( "(%p)->(0x%08lx,%p,%p,0x%08lx): stub\n", This, session, enumplayercb, context, flags );
    return E_NOTIMPL;
}

static HRESULT WINAPI IDirectPlayImpl_EnumSessions( IDirectPlay *iface, DPSESSIONDESC *sdesc,
        DWORD timeout, LPDPENUMSESSIONSCALLBACK enumsessioncb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    FIXME( "(%p)->(%p,%lu,%p,%p,0x%08lx): stub\n", This, sdesc, timeout, enumsessioncb, context,
            flags );
    return E_NOTIMPL;
}

static HRESULT WINAPI IDirectPlayImpl_GetCaps( IDirectPlay *iface, DPCAPS *caps )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    FIXME( "(%p)->(%p): stub\n", This, caps );
    return E_NOTIMPL;
}

static HRESULT WINAPI IDirectPlayImpl_GetMessageCount( IDirectPlay *iface, DPID player,
        DWORD *count )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    FIXME( "(%p)->(0x%08lx,%p): stub\n", This, player, count );
    return E_NOTIMPL;
}

static HRESULT WINAPI IDirectPlayImpl_GetPlayerCaps( IDirectPlay *iface, DPID player, DPCAPS *caps )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    FIXME( "(%p)->(0x%08lx,%p): stub\n", This, player, caps );
    return E_NOTIMPL;
}

static HRESULT WINAPI IDirectPlayImpl_GetPlayerName( IDirectPlay *iface, DPID player, LPSTR name,
        DWORD *size_name, LPSTR fullname, DWORD *size_fullname )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    FIXME( "(%p)->(0x%08lx,%p,%p,%p,%p): stub\n", This, player, name, size_name, fullname,
            size_fullname );
    return E_NOTIMPL;
}

static HRESULT WINAPI IDirectPlayImpl_Initialize( IDirectPlay *iface, GUID *guid )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    FIXME( "(%p)->(%p): stub\n", This, guid );
    return E_NOTIMPL;
}

static HRESULT WINAPI IDirectPlayImpl_Open( IDirectPlay *iface, DPSESSIONDESC *sdesc )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    FIXME( "(%p)->(%p): stub\n", This, sdesc );
    return E_NOTIMPL;
}

static HRESULT WINAPI IDirectPlayImpl_Receive( IDirectPlay *iface, DPID *from, DPID *to,
        DWORD flags, void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    FIXME( "(%p)->(%p,%p,0x%08lx,%p,%p): stub\n", This, from, to, flags, data, size );
    return E_NOTIMPL;
}

static HRESULT WINAPI IDirectPlayImpl_SaveSession( IDirectPlay *iface, LPSTR reserved )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    FIXME( "(%p)->(%p): stub\n", This, reserved );
    return E_NOTIMPL;
}

static HRESULT WINAPI IDirectPlayImpl_Send( IDirectPlay *iface, DPID from, DPID to, DWORD flags,
        void *data, DWORD size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    FIXME( "(%p)->(0x%08lx,0x%08lx,0x%08lx,%p,%lu): stub\n", This, from, to, flags, data, size );
    return E_NOTIMPL;
}

static HRESULT WINAPI IDirectPlayImpl_SetPlayerName( IDirectPlay *iface, DPID player, LPSTR name,
        LPSTR fullname )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    FIXME( "(%p)->(0x%08lx,%s,%s): stub\n", This, player, debugstr_a( name ),
            debugstr_a ( fullname ) );
    return E_NOTIMPL;
}

static const IDirectPlayVtbl dp_vt =
{
    IDirectPlayImpl_QueryInterface,
    IDirectPlayImpl_AddRef,
    IDirectPlayImpl_Release,
    IDirectPlayImpl_AddPlayerToGroup,
    IDirectPlayImpl_Close,
    IDirectPlayImpl_CreatePlayer,
    IDirectPlayImpl_CreateGroup,
    IDirectPlayImpl_DeletePlayerFromGroup,
    IDirectPlayImpl_DestroyPlayer,
    IDirectPlayImpl_DestroyGroup,
    IDirectPlayImpl_EnableNewPlayers,
    IDirectPlayImpl_EnumGroupPlayers,
    IDirectPlayImpl_EnumGroups,
    IDirectPlayImpl_EnumPlayers,
    IDirectPlayImpl_EnumSessions,
    IDirectPlayImpl_GetCaps,
    IDirectPlayImpl_GetMessageCount,
    IDirectPlayImpl_GetPlayerCaps,
    IDirectPlayImpl_GetPlayerName,
    IDirectPlayImpl_Initialize,
    IDirectPlayImpl_Open,
    IDirectPlayImpl_Receive,
    IDirectPlayImpl_SaveSession,
    IDirectPlayImpl_Send,
    IDirectPlayImpl_SetPlayerName,
};


static HRESULT WINAPI IDirectPlay2AImpl_QueryInterface( IDirectPlay2A *iface, REFIID riid,
        void **ppv )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_QueryInterface( &This->IDirectPlay4_iface, riid, ppv );
}

static HRESULT WINAPI IDirectPlay2Impl_QueryInterface( IDirectPlay2 *iface, REFIID riid,
        void **ppv )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_QueryInterface( &This->IDirectPlay4_iface, riid, ppv );
}

static HRESULT WINAPI IDirectPlay3AImpl_QueryInterface( IDirectPlay3A *iface, REFIID riid,
        void **ppv )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_QueryInterface( &This->IDirectPlay4_iface, riid, ppv );
}

static HRESULT WINAPI IDirectPlay3Impl_QueryInterface( IDirectPlay3 *iface, REFIID riid,
        void **ppv )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_QueryInterface( &This->IDirectPlay4_iface, riid, ppv );
}

static HRESULT WINAPI IDirectPlay4AImpl_QueryInterface( IDirectPlay4A *iface, REFIID riid,
        void **ppv )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_QueryInterface( &This->IDirectPlay4_iface, riid, ppv );
}

static HRESULT WINAPI IDirectPlay4Impl_QueryInterface( IDirectPlay4 *iface, REFIID riid,
        void **ppv )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );

    if ( IsEqualGUID( &IID_IUnknown, riid ) )
    {
        TRACE( "(%p)->(IID_IUnknown %p)\n", This, ppv );
        *ppv = &This->IDirectPlay_iface;
    }
    else if ( IsEqualGUID( &IID_IDirectPlay, riid ) )
    {
        TRACE( "(%p)->(IID_IDirectPlay %p)\n", This, ppv );
        *ppv = &This->IDirectPlay_iface;
    }
    else if ( IsEqualGUID( &IID_IDirectPlay2A, riid ) )
    {
        TRACE( "(%p)->(IID_IDirectPlay2A %p)\n", This, ppv );
        *ppv = &This->IDirectPlay2A_iface;
    }
    else if ( IsEqualGUID( &IID_IDirectPlay2, riid ) )
    {
        TRACE( "(%p)->(IID_IDirectPlay2 %p)\n", This, ppv );
        *ppv = &This->IDirectPlay2_iface;
    }
    else if ( IsEqualGUID( &IID_IDirectPlay3A, riid ) )
    {
        TRACE( "(%p)->(IID_IDirectPlay3A %p)\n", This, ppv );
        *ppv = &This->IDirectPlay3A_iface;
    }
    else if ( IsEqualGUID( &IID_IDirectPlay3, riid ) )
    {
        TRACE( "(%p)->(IID_IDirectPlay3 %p)\n", This, ppv );
        *ppv = &This->IDirectPlay3_iface;
    }
    else if ( IsEqualGUID( &IID_IDirectPlay4A, riid ) )
    {
        TRACE( "(%p)->(IID_IDirectPlay4A %p)\n", This, ppv );
        *ppv = &This->IDirectPlay4A_iface;
    }
    else if ( IsEqualGUID( &IID_IDirectPlay4, riid ) )
    {
        TRACE( "(%p)->(IID_IDirectPlay4 %p)\n", This, ppv );
        *ppv = &This->IDirectPlay4_iface;
    }
    else
    {
        WARN("(%p)->(%s %p)\n", This, debugstr_guid(riid), ppv);
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown*)*ppv);
    return S_OK;
}

static ULONG WINAPI IDirectPlay2AImpl_AddRef( IDirectPlay2A *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    ULONG ref = InterlockedIncrement( &This->ref2A );

    TRACE( "(%p) ref2A=%ld\n", This, ref );

    if ( ref == 1 )
        InterlockedIncrement( &This->numIfaces );

    return ref;
}

static ULONG WINAPI IDirectPlay2Impl_AddRef( IDirectPlay2 *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    ULONG ref = InterlockedIncrement( &This->ref2 );

    TRACE( "(%p) ref2=%ld\n", This, ref );

    if ( ref == 1 )
        InterlockedIncrement( &This->numIfaces );

    return ref;
}

static ULONG WINAPI IDirectPlay3AImpl_AddRef( IDirectPlay3A *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    ULONG ref = InterlockedIncrement( &This->ref3A );

    TRACE( "(%p) ref3A=%ld\n", This, ref );

    if ( ref == 1 )
        InterlockedIncrement( &This->numIfaces );

    return ref;
}

static ULONG WINAPI IDirectPlay3Impl_AddRef( IDirectPlay3 *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    ULONG ref = InterlockedIncrement( &This->ref3 );

    TRACE( "(%p) ref3=%ld\n", This, ref );

    if ( ref == 1 )
        InterlockedIncrement( &This->numIfaces );

    return ref;
}

static ULONG WINAPI IDirectPlay4AImpl_AddRef(IDirectPlay4A *iface)
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    ULONG ref = InterlockedIncrement( &This->ref4A );

    TRACE( "(%p) ref4A=%ld\n", This, ref );

    if ( ref == 1 )
        InterlockedIncrement( &This->numIfaces );

    return ref;
}

static ULONG WINAPI IDirectPlay4Impl_AddRef(IDirectPlay4 *iface)
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    ULONG ref = InterlockedIncrement( &This->ref4 );

    TRACE( "(%p) ref4=%ld\n", This, ref );

    if ( ref == 1 )
        InterlockedIncrement( &This->numIfaces );

    return ref;
}

static ULONG WINAPI IDirectPlay2AImpl_Release( IDirectPlay2A *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    ULONG ref = InterlockedDecrement( &This->ref2A );

    TRACE( "(%p) ref2A=%ld\n", This, ref );

    if ( !ref && !InterlockedDecrement( &This->numIfaces ) )
        dplay_destroy( This );

    return ref;
}

static ULONG WINAPI IDirectPlay2Impl_Release( IDirectPlay2 *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    ULONG ref = InterlockedDecrement( &This->ref2 );

    TRACE( "(%p) ref2=%ld\n", This, ref );

    if ( !ref && !InterlockedDecrement( &This->numIfaces ) )
        dplay_destroy( This );

    return ref;
}

static ULONG WINAPI IDirectPlay3AImpl_Release( IDirectPlay3A *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    ULONG ref = InterlockedDecrement( &This->ref3A );

    TRACE( "(%p) ref3A=%ld\n", This, ref );

    if ( !ref && !InterlockedDecrement( &This->numIfaces ) )
        dplay_destroy( This );

    return ref;
}

static ULONG WINAPI IDirectPlay3Impl_Release( IDirectPlay3 *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    ULONG ref = InterlockedDecrement( &This->ref3 );

    TRACE( "(%p) ref3=%ld\n", This, ref );

    if ( !ref && !InterlockedDecrement( &This->numIfaces ) )
        dplay_destroy( This );

    return ref;
}

static ULONG WINAPI IDirectPlay4AImpl_Release(IDirectPlay4A *iface)
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    ULONG ref = InterlockedDecrement( &This->ref4A );

    TRACE( "(%p) ref4A=%ld\n", This, ref );

    if ( !ref && !InterlockedDecrement( &This->numIfaces ) )
        dplay_destroy( This );

    return ref;
}

static ULONG WINAPI IDirectPlay4Impl_Release(IDirectPlay4 *iface)
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    ULONG ref = InterlockedDecrement( &This->ref4 );

    TRACE( "(%p) ref4=%ld\n", This, ref );

    if ( !ref && !InterlockedDecrement( &This->numIfaces ) )
        dplay_destroy( This );

    return ref;
}

static HRESULT WINAPI IDirectPlay2AImpl_AddPlayerToGroup( IDirectPlay2A *iface, DPID group,
        DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_AddPlayerToGroup( &This->IDirectPlay4A_iface, group, player );
}

static HRESULT WINAPI IDirectPlay2Impl_AddPlayerToGroup( IDirectPlay2 *iface, DPID group,
        DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_AddPlayerToGroup( &This->IDirectPlay4_iface, group, player );
}

static HRESULT WINAPI IDirectPlay3AImpl_AddPlayerToGroup( IDirectPlay3A *iface, DPID group,
        DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_AddPlayerToGroup( &This->IDirectPlay4_iface, group, player );
}

static HRESULT WINAPI IDirectPlay3Impl_AddPlayerToGroup( IDirectPlay3 *iface, DPID group,
        DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_AddPlayerToGroup( &This->IDirectPlay4_iface, group, player );
}

static HRESULT WINAPI IDirectPlay4AImpl_AddPlayerToGroup( IDirectPlay4A *iface, DPID group,
        DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_AddPlayerToGroup( &This->IDirectPlay4_iface, group, player );
}

static HRESULT WINAPI IDirectPlay4Impl_AddPlayerToGroup( IDirectPlay4 *iface, DPID group,
        DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    lpGroupData  gdata;
    lpPlayerList plist;
    lpPlayerList newplist;

    TRACE( "(%p)->(0x%08lx,0x%08lx)\n", This, group, player );

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;

    /* Find the group */
    if ( ( gdata = DP_FindAnyGroup( This, group ) ) == NULL )
        return DPERR_INVALIDGROUP;

    /* Find the player */
    if ( ( plist = DP_FindPlayer( This, player ) ) == NULL )
        return DPERR_INVALIDPLAYER;

    /* Create a player list (ie "shortcut" ) */
    newplist = calloc( 1, sizeof( *newplist ) );
    if ( !newplist )
        return DPERR_CANTADDPLAYER;

    /* Add the shortcut */
    plist->lpPData->uRef++;
    newplist->lpPData = plist->lpPData;

    /* Add the player to the list of players for this group */
    DPQ_INSERT(gdata->players, newplist, players);

    /* Let the SP know that we've added a player to the group */
    if ( This->dp2->spData.lpCB->AddPlayerToGroup )
    {
        DPSP_ADDPLAYERTOGROUPDATA data;

        TRACE( "Calling SP AddPlayerToGroup\n" );

        data.idPlayer = player;
        data.idGroup  = group;
        data.lpISP    = This->dp2->spData.lpISP;

        (*This->dp2->spData.lpCB->AddPlayerToGroup)( &data );
    }

    /* Inform all other peers of the addition of player to the group. If there are
     * no peers keep this event quiet.
     * Also, if this event was the result of another machine sending it to us,
     * don't bother rebroadcasting it.
     */
    if ( This->dp2->lpSessionDesc &&
            ( This->dp2->lpSessionDesc->dwFlags & DPSESSION_MULTICASTSERVER ) )
    {
        DPMSG_ADDPLAYERTOGROUP msg;
        msg.dwType = DPSYS_ADDPLAYERTOGROUP;

        msg.dpIdGroup  = group;
        msg.dpIdPlayer = player;

        /* FIXME: Correct to just use send effectively? */
        /* FIXME: Should size include data w/ message or just message "header" */
        /* FIXME: Check return code */
        IDirectPlayX_SendEx( iface, DPID_SERVERPLAYER, DPID_ALLPLAYERS, 0, &msg, sizeof( msg ),
                0, 0, NULL, NULL );
    }

    return DP_OK;
}

static HRESULT WINAPI IDirectPlay2AImpl_Close( IDirectPlay2A *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_Close( &This->IDirectPlay4A_iface );
}

static HRESULT WINAPI IDirectPlay2Impl_Close( IDirectPlay2 *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_Close( &This->IDirectPlay4_iface );
}

static HRESULT WINAPI IDirectPlay3AImpl_Close( IDirectPlay3A *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_Close( &This->IDirectPlay4_iface );
}

static HRESULT WINAPI IDirectPlay3Impl_Close( IDirectPlay3 *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_Close( &This->IDirectPlay4_iface );
}

static HRESULT WINAPI IDirectPlay4AImpl_Close( IDirectPlay4A *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_Close( &This->IDirectPlay4_iface);
}

static HRESULT WINAPI IDirectPlay4Impl_Close( IDirectPlay4 *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    HRESULT hr = DP_OK;

    TRACE( "(%p)\n", This );

    /* FIXME: Need to find a new host I assume (how?) */
    /* FIXME: Need to destroy all local groups */
    /* FIXME: Need to migrate all remotely visible players to the new host */

    /* Invoke the SP callback to inform of session close */
    if( This->dp2->spData.lpCB->CloseEx )
    {
        DPSP_CLOSEDATA data;

        TRACE( "Calling SP CloseEx\n" );
        data.lpISP = This->dp2->spData.lpISP;
        hr = (*This->dp2->spData.lpCB->CloseEx)( &data );
    }
    else if ( This->dp2->spData.lpCB->Close ) /* Try obsolete version */
    {
        TRACE( "Calling SP Close (obsolete interface)\n" );
        hr = (*This->dp2->spData.lpCB->Close)();
    }

    return hr;
}

static lpGroupData DP_CreateGroup( IDirectPlayImpl *This, const DPID *lpid, const DPNAME *lpName,
        DWORD dwFlags, DPID idParent, BOOL bAnsi )
{
  lpGroupData lpGData;

  /* Allocate the new space and add to end of high level group list */
  lpGData = calloc( 1, sizeof( *lpGData ) );

  if( lpGData == NULL )
  {
    return NULL;
  }

  DPQ_INIT(lpGData->groups);
  DPQ_INIT(lpGData->players);

  /* Set the desired player ID - no sanity checking to see if it exists */
  lpGData->dpid = *lpid;

  DP_CopyDPNAMEStruct( &lpGData->name, lpName, bAnsi );

  /* FIXME: Should we check that the parent exists? */
  lpGData->parent  = idParent;

  /* FIXME: Should we validate the dwFlags? */
  lpGData->dwFlags = dwFlags;

  TRACE( "Created group id 0x%08lx\n", *lpid );

  return lpGData;
}

/* This method assumes that all links to it are already deleted */
static void DP_DeleteGroup( IDirectPlayImpl *This, DPID dpid )
{
  lpGroupList lpGList;

  TRACE( "(%p)->(0x%08lx)\n", This, dpid );

  DPQ_REMOVE_ENTRY( This->dp2->lpSysGroup->groups, groups, lpGData->dpid, ==, dpid, lpGList );

  if( lpGList == NULL )
  {
    ERR( "DPID 0x%08lx not found\n", dpid );
    return;
  }

  if( --(lpGList->lpGData->uRef) )
  {
    FIXME( "Why is this not the last reference to group?\n" );
    DebugBreak();
  }

  /* Delete player */
  DP_DeleteDPNameStruct( &lpGList->lpGData->name );
  free( lpGList->lpGData );

  /* Remove and Delete Player List object */
  free( lpGList );

}

static lpGroupData DP_FindAnyGroup( IDirectPlayImpl *This, DPID dpid )
{
  lpGroupList lpGroups;

  TRACE( "(%p)->(0x%08lx)\n", This, dpid );

  if( dpid == DPID_SYSTEM_GROUP )
  {
    return This->dp2->lpSysGroup;
  }
  else
  {
    DPQ_FIND_ENTRY( This->dp2->lpSysGroup->groups, groups, lpGData->dpid, ==, dpid, lpGroups );
  }

  if( lpGroups == NULL )
  {
    return NULL;
  }

  return lpGroups->lpGData;
}

static HRESULT DP_IF_CreateGroup( IDirectPlayImpl *This, void *lpMsgHdr, DPID *lpidGroup,
        DPNAME *lpGroupName, void *lpData, DWORD dwDataSize, DWORD dwFlags, BOOL bAnsi )
{
  lpGroupData lpGData;

  TRACE( "(%p)->(%p,%p,%p,%p,0x%08lx,0x%08lx,%u)\n",
         This, lpMsgHdr, lpidGroup, lpGroupName, lpData, dwDataSize,
         dwFlags, bAnsi );

  if( This->dp2->connectionInitialized == NO_PROVIDER )
  {
    return DPERR_UNINITIALIZED;
  }

  /* If the name is not specified, we must provide one */
  if( DPID_UNKNOWN == *lpidGroup )
  {
    /* If we are the name server, we decide on the group ids. If not, we
     * must ask for one before attempting a creation.
     */
    if( This->dp2->bHostInterface )
    {
      *lpidGroup = DP_NextObjectId();
    }
    else
    {
      *lpidGroup = DP_GetRemoteNextObjectId();
    }
  }

  lpGData = DP_CreateGroup( This, lpidGroup, lpGroupName, dwFlags,
                            DPID_NOPARENT_GROUP, bAnsi );

  if( lpGData == NULL )
  {
    return DPERR_CANTADDPLAYER; /* yes player not group */
  }

  if( DPID_SYSTEM_GROUP == *lpidGroup )
  {
    This->dp2->lpSysGroup = lpGData;
    TRACE( "Inserting system group\n" );
  }
  else
  {
    /* Insert into the system group */
    lpGroupList lpGroup = calloc( 1, sizeof( *lpGroup ) );
    lpGroup->lpGData = lpGData;

    DPQ_INSERT( This->dp2->lpSysGroup->groups, lpGroup, groups );
  }

  /* Something is now referencing this data */
  lpGData->uRef++;

  /* Set all the important stuff for the group */
  DP_SetGroupData( lpGData, DPSET_REMOTE, lpData, dwDataSize );

  /* FIXME: We should only create the system group if GetCaps returns
   *        DPCAPS_GROUPOPTIMIZED.
   */

  /* Let the SP know that we've created this group */
  if( This->dp2->spData.lpCB->CreateGroup )
  {
    DPSP_CREATEGROUPDATA data;
    DWORD dwCreateFlags = 0;

    TRACE( "Calling SP CreateGroup\n" );

    if( *lpidGroup == DPID_NOPARENT_GROUP )
      dwCreateFlags |= DPLAYI_GROUP_SYSGROUP;

    if( lpMsgHdr == NULL )
      dwCreateFlags |= DPLAYI_PLAYER_PLAYERLOCAL;

    if( dwFlags & DPGROUP_HIDDEN )
      dwCreateFlags |= DPLAYI_GROUP_HIDDEN;

    data.idGroup           = *lpidGroup;
    data.dwFlags           = dwCreateFlags;
    data.lpSPMessageHeader = lpMsgHdr;
    data.lpISP             = This->dp2->spData.lpISP;

    (*This->dp2->spData.lpCB->CreateGroup)( &data );
  }

  /* Inform all other peers of the creation of a new group. If there are
   * no peers keep this event quiet.
   * Also if this message was sent to us, don't rebroadcast.
   */
  if( ( lpMsgHdr == NULL ) &&
      This->dp2->lpSessionDesc &&
      ( This->dp2->lpSessionDesc->dwFlags & DPSESSION_MULTICASTSERVER ) )
  {
    DPMSG_CREATEPLAYERORGROUP msg;
    msg.dwType = DPSYS_CREATEPLAYERORGROUP;

    msg.dwPlayerType     = DPPLAYERTYPE_GROUP;
    msg.dpId             = *lpidGroup;
    msg.dwCurrentPlayers = 0; /* FIXME: Incorrect? */
    msg.lpData           = lpData;
    msg.dwDataSize       = dwDataSize;
    msg.dpnName          = *lpGroupName;
    msg.dpIdParent       = DPID_NOPARENT_GROUP;
    msg.dwFlags          = DPMSG_CREATEGROUP_DWFLAGS( dwFlags );

    /* FIXME: Correct to just use send effectively? */
    /* FIXME: Should size include data w/ message or just message "header" */
    /* FIXME: Check return code */
    IDirectPlayX_SendEx( &This->IDirectPlay4_iface, DPID_SERVERPLAYER, DPID_ALLPLAYERS, 0, &msg,
            sizeof( msg ), 0, 0, NULL, NULL );
  }

  return DP_OK;
}

static HRESULT WINAPI IDirectPlay2AImpl_CreateGroup( IDirectPlay2A *iface, DPID *lpidGroup,
        DPNAME *name, void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_CreateGroup( &This->IDirectPlay4A_iface, lpidGroup, name, data, size,
            flags );
}

static HRESULT WINAPI IDirectPlay2Impl_CreateGroup( IDirectPlay2 *iface, DPID *lpidGroup,
        DPNAME *name, void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_CreateGroup( &This->IDirectPlay4_iface, lpidGroup, name, data, size,
            flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_CreateGroup( IDirectPlay3A *iface, DPID *group,
        DPNAME *name, void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_CreateGroup( &This->IDirectPlay4_iface, group, name, data, size,
            flags );
}

static HRESULT WINAPI IDirectPlay3Impl_CreateGroup( IDirectPlay3 *iface, DPID *lpidGroup,
        DPNAME *name, void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_CreateGroup( &This->IDirectPlay4_iface, lpidGroup, name, data, size,
            flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_CreateGroup( IDirectPlay4A *iface, DPID *lpidGroup,
        DPNAME *lpGroupName, void *lpData, DWORD dwDataSize, DWORD dwFlags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );

    *lpidGroup = DPID_UNKNOWN;

    return DP_IF_CreateGroup( This, NULL, lpidGroup, lpGroupName, lpData, dwDataSize, dwFlags,
            TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_CreateGroup( IDirectPlay4 *iface, DPID *lpidGroup,
        DPNAME *lpGroupName, void *lpData, DWORD dwDataSize, DWORD dwFlags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );

    *lpidGroup = DPID_UNKNOWN;

    return DP_IF_CreateGroup( This, NULL, lpidGroup, lpGroupName, lpData, dwDataSize, dwFlags,
            FALSE );
}


static void
DP_SetGroupData( lpGroupData lpGData, DWORD dwFlags,
                 LPVOID lpData, DWORD dwDataSize )
{
  /* Clear out the data with this player */
  if( dwFlags & DPSET_LOCAL )
  {
    if ( lpGData->dwLocalDataSize != 0 )
    {
      free( lpGData->lpLocalData );
      lpGData->lpLocalData = NULL;
      lpGData->dwLocalDataSize = 0;
    }
  }
  else
  {
    if( lpGData->dwRemoteDataSize != 0 )
    {
      free( lpGData->lpRemoteData );
      lpGData->lpRemoteData = NULL;
      lpGData->dwRemoteDataSize = 0;
    }
  }

  /* Reallocate for new data */
  if( lpData != NULL )
  {
    if( dwFlags & DPSET_LOCAL )
    {
      lpGData->lpLocalData     = lpData;
      lpGData->dwLocalDataSize = dwDataSize;
    }
    else
    {
      lpGData->lpRemoteData = malloc( dwDataSize );
      CopyMemory( lpGData->lpRemoteData, lpData, dwDataSize );
      lpGData->dwRemoteDataSize = dwDataSize;
    }
  }

}

/* This function will just create the storage for the new player.  */
static lpPlayerData DP_CreatePlayer( IDirectPlayImpl *This, DPID *lpid, DPNAME *lpName,
        DWORD dwFlags, HANDLE hEvent, BOOL bAnsi )
{
  lpPlayerData lpPData;

  TRACE( "(%p)->(%p,%p,%u)\n", This, lpid, lpName, bAnsi );

  /* Allocate the storage for the player and associate it with list element */
  lpPData = calloc( 1, sizeof( *lpPData ) );
  if( lpPData == NULL )
  {
    return NULL;
  }

  /* Set the desired player ID */
  lpPData->dpid = *lpid;

  DP_CopyDPNAMEStruct( &lpPData->name, lpName, bAnsi );

  lpPData->dwFlags = dwFlags;

  /* If we were given an event handle, duplicate it */
  if( hEvent != 0 )
  {
    if( !DuplicateHandle( GetCurrentProcess(), hEvent,
                          GetCurrentProcess(), &lpPData->hEvent,
                          0, FALSE, DUPLICATE_SAME_ACCESS )
      )
    {
      /* FIXME: Memory leak */
      ERR( "Can't duplicate player msg handle %p\n", hEvent );
    }
  }

  /* Initialize the SP data section */
  lpPData->lpSPPlayerData = DPSP_CreateSPPlayerData();

  TRACE( "Created player id 0x%08lx\n", *lpid );

  if( ~dwFlags & DPLAYI_PLAYER_SYSPLAYER )
    This->dp2->lpSessionDesc->dwCurrentPlayers++;

  return lpPData;
}

/* Delete the contents of the DPNAME struct */
static void
DP_DeleteDPNameStruct( LPDPNAME lpDPName )
{
  free( lpDPName->lpszShortNameA );
  free( lpDPName->lpszLongNameA );
}

/* This method assumes that all links to it are already deleted */
static void DP_DeletePlayer( IDirectPlayImpl *This, DPID dpid )
{
  lpPlayerList lpPList;

  TRACE( "(%p)->(0x%08lx)\n", This, dpid );

  DPQ_REMOVE_ENTRY( This->dp2->lpSysGroup->players, players, lpPData->dpid, ==, dpid, lpPList );

  if( lpPList == NULL )
  {
    ERR( "DPID 0x%08lx not found\n", dpid );
    return;
  }

  /* Verify that this is the last reference to the data */
  if( --(lpPList->lpPData->uRef) )
  {
    FIXME( "Why is this not the last reference to player?\n" );
    DebugBreak();
  }

  /* Delete player */
  DP_DeleteDPNameStruct( &lpPList->lpPData->name );

  CloseHandle( lpPList->lpPData->hEvent );
  free( lpPList->lpPData );

  /* Delete Player List object */
  free( lpPList );
}

static lpPlayerList DP_FindPlayer( IDirectPlayImpl *This, DPID dpid )
{
  lpPlayerList lpPlayers;

  TRACE( "(%p)->(0x%08lx)\n", This, dpid );

  if(This->dp2->lpSysGroup == NULL)
    return NULL;

  DPQ_FIND_ENTRY( This->dp2->lpSysGroup->players, players, lpPData->dpid, ==, dpid, lpPlayers );

  return lpPlayers;
}

/* Basic area for Dst must already be allocated */
static BOOL DP_CopyDPNAMEStruct( LPDPNAME lpDst, const DPNAME *lpSrc, BOOL bAnsi )
{
  if( lpSrc == NULL )
  {
    ZeroMemory( lpDst, sizeof( *lpDst ) );
    lpDst->dwSize = sizeof( *lpDst );
    return TRUE;
  }

  if( lpSrc->dwSize != sizeof( *lpSrc) )
  {
    return FALSE;
  }

  /* Delete any existing pointers */
  free( lpDst->lpszShortNameA );
  free( lpDst->lpszLongNameA );

  /* Copy as required */
  CopyMemory( lpDst, lpSrc, lpSrc->dwSize );

  if( bAnsi )
  {
    lpDst->lpszShortNameA = strdup( lpSrc->lpszShortNameA );
    lpDst->lpszLongNameA = strdup( lpSrc->lpszLongNameA );
  }
  else
  {
    lpDst->lpszShortName = wcsdup( lpSrc->lpszShortName );
    lpDst->lpszLongName = wcsdup( lpSrc->lpszLongName );
  }

  return TRUE;
}

static void
DP_SetPlayerData( lpPlayerData lpPData, DWORD dwFlags,
                  LPVOID lpData, DWORD dwDataSize )
{
  /* Clear out the data with this player */
  if( dwFlags & DPSET_LOCAL )
  {
    if ( lpPData->dwLocalDataSize != 0 )
    {
      free( lpPData->lpLocalData );
      lpPData->lpLocalData = NULL;
      lpPData->dwLocalDataSize = 0;
    }
  }
  else
  {
    if( lpPData->dwRemoteDataSize != 0 )
    {
      free( lpPData->lpRemoteData );
      lpPData->lpRemoteData = NULL;
      lpPData->dwRemoteDataSize = 0;
    }
  }

  /* Reallocate for new data */
  if( lpData != NULL )
  {

    if( dwFlags & DPSET_LOCAL )
    {
      lpPData->lpLocalData     = lpData;
      lpPData->dwLocalDataSize = dwDataSize;
    }
    else
    {
      lpPData->lpRemoteData = malloc( dwDataSize );
      CopyMemory( lpPData->lpRemoteData, lpData, dwDataSize );
      lpPData->dwRemoteDataSize = dwDataSize;
    }
  }

}

/* Note: lpMsgHdr is NULL for local creation, non NULL for remote creation */
static HRESULT DP_IF_CreatePlayer( IDirectPlayImpl *This, void *lpMsgHdr, DPID *lpidPlayer,
        DPNAME *lpPlayerName, HANDLE hEvent, void *lpData, DWORD dwDataSize, DWORD dwFlags,
        BOOL bAnsi )
{
  HRESULT hr = DP_OK;
  lpPlayerData lpPData;
  lpPlayerList lpPList;
  DWORD dwCreateFlags = 0;

  TRACE( "(%p)->(%p,%p,%p,%p,0x%08lx,0x%08lx,%u)\n",
         This, lpidPlayer, lpPlayerName, hEvent, lpData,
         dwDataSize, dwFlags, bAnsi );
  if( This->dp2->connectionInitialized == NO_PROVIDER )
  {
    return DPERR_UNINITIALIZED;
  }

  if( dwFlags == 0 )
  {
    dwFlags = DPPLAYER_SPECTATOR;
  }

  if( lpidPlayer == NULL )
  {
    return DPERR_INVALIDPARAMS;
  }


  /* Determine the creation flags for the player. These will be passed
   * to the name server if requesting a player id and to the SP when
   * informing it of the player creation
   */
  {
    if( dwFlags & DPPLAYER_SERVERPLAYER )
    {
      if( *lpidPlayer == DPID_SERVERPLAYER )
      {
        /* Server player for the host interface */
        dwCreateFlags |= DPLAYI_PLAYER_APPSERVER;
      }
      else if( *lpidPlayer == DPID_NAME_SERVER )
      {
        /* Name server - master of everything */
        dwCreateFlags |= (DPLAYI_PLAYER_NAMESRVR|DPLAYI_PLAYER_SYSPLAYER);
      }
      else
      {
        /* Server player for a non host interface */
        dwCreateFlags |= DPLAYI_PLAYER_SYSPLAYER;
      }
    }

    if( lpMsgHdr == NULL )
      dwCreateFlags |= DPLAYI_PLAYER_PLAYERLOCAL;
  }

  /* Verify we know how to handle all the flags */
  if( !( ( dwFlags & DPPLAYER_SERVERPLAYER ) ||
         ( dwFlags & DPPLAYER_SPECTATOR )
       )
    )
  {
    /* Assume non fatal failure */
    ERR( "unknown dwFlags = 0x%08lx\n", dwFlags );
  }

  /* If the name is not specified, we must provide one */
  if( *lpidPlayer == DPID_UNKNOWN )
  {
    /* If we are the session master, we dish out the group/player ids */
    if( This->dp2->bHostInterface )
    {
      *lpidPlayer = DP_NextObjectId();
    }
    else
    {
      hr = DP_MSG_SendRequestPlayerId( This, dwCreateFlags, lpidPlayer );

      if( FAILED(hr) )
      {
        ERR( "Request for ID failed: %s\n", DPLAYX_HresultToString( hr ) );
        return hr;
      }
    }
  }
  else
  {
    /* FIXME: Would be nice to perhaps verify that we don't already have
     *        this player.
     */
  }

  /* We pass creation flags, so we can distinguish sysplayers and not count them in the current
     player total */
  lpPData = DP_CreatePlayer( This, lpidPlayer, lpPlayerName, dwCreateFlags,
                             hEvent, bAnsi );
  /* Create the list object and link it in */
  lpPList = calloc( 1, sizeof( *lpPList ) );
  if( !lpPData || !lpPList )
  {
    free( lpPData );
    free( lpPList );
    return DPERR_CANTADDPLAYER;
  }

  lpPData->uRef = 1;
  lpPList->lpPData = lpPData;

  /* Add the player to the system group */
  DPQ_INSERT( This->dp2->lpSysGroup->players, lpPList, players );

  /* Update the information and send it to all players in the session */
  DP_SetPlayerData( lpPData, DPSET_REMOTE, lpData, dwDataSize );

  /* Let the SP know that we've created this player */
  if( This->dp2->spData.lpCB->CreatePlayer )
  {
    DPSP_CREATEPLAYERDATA data;

    data.idPlayer          = *lpidPlayer;
    data.dwFlags           = dwCreateFlags;
    data.lpSPMessageHeader = lpMsgHdr;
    data.lpISP             = This->dp2->spData.lpISP;

    TRACE( "Calling SP CreatePlayer 0x%08lx: dwFlags: 0x%08lx lpMsgHdr: %p\n",
           *lpidPlayer, data.dwFlags, data.lpSPMessageHeader );

    hr = (*This->dp2->spData.lpCB->CreatePlayer)( &data );
  }

  if( FAILED(hr) )
  {
    ERR( "Failed to create player with sp: %s\n", DPLAYX_HresultToString(hr) );
    return hr;
  }

  /* Now let the SP know that this player is a member of the system group */
  if( This->dp2->spData.lpCB->AddPlayerToGroup )
  {
    DPSP_ADDPLAYERTOGROUPDATA data;

    data.idPlayer = *lpidPlayer;
    data.idGroup  = DPID_SYSTEM_GROUP;
    data.lpISP    = This->dp2->spData.lpISP;

    TRACE( "Calling SP AddPlayerToGroup (sys group)\n" );

    hr = (*This->dp2->spData.lpCB->AddPlayerToGroup)( &data );
  }

  if( FAILED(hr) )
  {
    ERR( "Failed to add player to sys group with sp: %s\n",
         DPLAYX_HresultToString(hr) );
    return hr;
  }

#if 1
  if( !This->dp2->bHostInterface )
  {
    /* Let the name server know about the creation of this player */
    /* FIXME: Is this only to be done for the creation of a server player or
     *        is this used for regular players? If only for server players, move
     *        this call to DP_SecureOpen(...);
     */
#if 0
    TRACE( "Sending message to self to get my addr\n" );
    DP_MSG_ToSelf( This, *lpidPlayer ); /* This is a hack right now */
#endif

    hr = DP_MSG_ForwardPlayerCreation( This, *lpidPlayer);
  }
#else
  /* Inform all other peers of the creation of a new player. If there are
   * no peers keep this quiet.
   * Also, if this was a remote event, no need to rebroadcast it.
   */
  if( ( lpMsgHdr == NULL ) &&
      This->dp2->lpSessionDesc &&
      ( This->dp2->lpSessionDesc->dwFlags & DPSESSION_MULTICASTSERVER ) )
  {
    DPMSG_CREATEPLAYERORGROUP msg;
    msg.dwType = DPSYS_CREATEPLAYERORGROUP;

    msg.dwPlayerType     = DPPLAYERTYPE_PLAYER;
    msg.dpId             = *lpidPlayer;
    msg.dwCurrentPlayers = 0; /* FIXME: Incorrect */
    msg.lpData           = lpData;
    msg.dwDataSize       = dwDataSize;
    msg.dpnName          = *lpPlayerName;
    msg.dpIdParent       = DPID_NOPARENT_GROUP;
    msg.dwFlags          = DPMSG_CREATEPLAYER_DWFLAGS( dwFlags );

    /* FIXME: Correct to just use send effectively? */
    /* FIXME: Should size include data w/ message or just message "header" */
    /* FIXME: Check return code */
    hr = IDirectPlayX_SendEx( &This->IDirectPlay4_iface, DPID_SERVERPLAYER, DPID_ALLPLAYERS, 0,
            &msg, sizeof( msg ), 0, 0, NULL, NULL );
  }
#endif

  return hr;
}

static HRESULT WINAPI IDirectPlay2AImpl_CreatePlayer( IDirectPlay2A *iface, DPID *lpidPlayer,
        DPNAME *name, HANDLE event, void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_CreatePlayer( &This->IDirectPlay4A_iface, lpidPlayer, name, event, data,
            size, flags );
}

static HRESULT WINAPI IDirectPlay2Impl_CreatePlayer( IDirectPlay2 *iface, DPID *lpidPlayer,
        DPNAME *name, HANDLE event, void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_CreatePlayer( &This->IDirectPlay4_iface, lpidPlayer, name, event, data,
            size, flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_CreatePlayer( IDirectPlay3A *iface, DPID *lpidPlayer,
        DPNAME *name, HANDLE event, void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_CreatePlayer( &This->IDirectPlay4_iface, lpidPlayer, name, event, data,
            size, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_CreatePlayer( IDirectPlay3 *iface, DPID *lpidPlayer,
        DPNAME *name, HANDLE event, void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_CreatePlayer( &This->IDirectPlay4_iface, lpidPlayer, name, event, data,
            size, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_CreatePlayer( IDirectPlay4A *iface, DPID *lpidPlayer,
        DPNAME *lpPlayerName, HANDLE hEvent, void *lpData, DWORD dwDataSize, DWORD dwFlags )
{
  IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );

  if( lpidPlayer == NULL )
  {
    return DPERR_INVALIDPARAMS;
  }

  if( dwFlags & DPPLAYER_SERVERPLAYER )
  {
    *lpidPlayer = DPID_SERVERPLAYER;
  }
  else
  {
    *lpidPlayer = DPID_UNKNOWN;
  }

  return DP_IF_CreatePlayer( This, NULL, lpidPlayer, lpPlayerName, hEvent,
                           lpData, dwDataSize, dwFlags, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_CreatePlayer( IDirectPlay4 *iface, DPID *lpidPlayer,
        DPNAME *lpPlayerName, HANDLE hEvent, void *lpData, DWORD dwDataSize, DWORD dwFlags )
{
  IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );

  if( lpidPlayer == NULL )
  {
    return DPERR_INVALIDPARAMS;
  }

  if( dwFlags & DPPLAYER_SERVERPLAYER )
  {
    *lpidPlayer = DPID_SERVERPLAYER;
  }
  else
  {
    *lpidPlayer = DPID_UNKNOWN;
  }

  return DP_IF_CreatePlayer( This, NULL, lpidPlayer, lpPlayerName, hEvent,
                           lpData, dwDataSize, dwFlags, FALSE );
}

static DPID DP_GetRemoteNextObjectId(void)
{
  FIXME( ":stub\n" );

  /* Hack solution */
  return DP_NextObjectId();
}

static HRESULT WINAPI IDirectPlay2AImpl_DeletePlayerFromGroup( IDirectPlay2A *iface, DPID group,
        DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_DeletePlayerFromGroup( &This->IDirectPlay4A_iface, group, player );
}

static HRESULT WINAPI IDirectPlay2Impl_DeletePlayerFromGroup( IDirectPlay2 *iface, DPID group,
        DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_DeletePlayerFromGroup( &This->IDirectPlay4_iface, group, player );
}

static HRESULT WINAPI IDirectPlay3AImpl_DeletePlayerFromGroup( IDirectPlay3A *iface, DPID group,
        DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_DeletePlayerFromGroup( &This->IDirectPlay4_iface, group, player );
}

static HRESULT WINAPI IDirectPlay3Impl_DeletePlayerFromGroup( IDirectPlay3 *iface, DPID group,
        DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_DeletePlayerFromGroup( &This->IDirectPlay4_iface, group, player );
}

static HRESULT WINAPI IDirectPlay4AImpl_DeletePlayerFromGroup( IDirectPlay4A *iface, DPID group,
        DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_DeletePlayerFromGroup( &This->IDirectPlay4_iface, group, player );
}

static HRESULT WINAPI IDirectPlay4Impl_DeletePlayerFromGroup( IDirectPlay4 *iface, DPID group,
        DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    HRESULT hr = DP_OK;

    lpGroupData  gdata;
    lpPlayerList plist;

    TRACE( "(%p)->(0x%08lx,0x%08lx)\n", This, group, player );

    /* Find the group */
    if ( ( gdata = DP_FindAnyGroup( This, group ) ) == NULL )
        return DPERR_INVALIDGROUP;

    /* Find the player */
    if ( DP_FindPlayer( This, player ) == NULL )
        return DPERR_INVALIDPLAYER;

    /* Remove the player shortcut from the group */
    DPQ_REMOVE_ENTRY( gdata->players, players, lpPData->dpid, ==, player, plist );

    if ( !plist )
        return DPERR_INVALIDPLAYER;

    /* One less reference */
    plist->lpPData->uRef--;

    /* Delete the Player List element */
    free( plist );

    /* Inform the SP if they care */
    if ( This->dp2->spData.lpCB->RemovePlayerFromGroup )
    {
        DPSP_REMOVEPLAYERFROMGROUPDATA data;

        TRACE( "Calling SP RemovePlayerFromGroup\n" );
        data.idPlayer = player;
        data.idGroup = group;
        data.lpISP = This->dp2->spData.lpISP;
        hr = (*This->dp2->spData.lpCB->RemovePlayerFromGroup)( &data );
    }

    /* Need to send a DELETEPLAYERFROMGROUP message */
    FIXME( "Need to send a message\n" );

    return hr;
}

typedef struct _DPRGOPContext
{
  IDirectPlayImpl   *This;
  BOOL              bAnsi;
  DPID              idGroup;
} DPRGOPContext, *lpDPRGOPContext;

static BOOL CALLBACK
cbRemoveGroupOrPlayer(
    DPID            dpId,
    DWORD           dwPlayerType,
    LPCDPNAME       lpName,
    DWORD           dwFlags,
    LPVOID          lpContext )
{
  lpDPRGOPContext lpCtxt = (lpDPRGOPContext)lpContext;

  TRACE( "Removing element:0x%08lx (type:0x%08lx) from element:0x%08lx\n",
           dpId, dwPlayerType, lpCtxt->idGroup );

  if( dwPlayerType == DPPLAYERTYPE_GROUP )
  {
    if ( FAILED( IDirectPlayX_DeleteGroupFromGroup( &lpCtxt->This->IDirectPlay4_iface,
                    lpCtxt->idGroup, dpId ) ) )
      ERR( "Unable to delete group 0x%08lx from group 0x%08lx\n", dpId, lpCtxt->idGroup );
  }
  else if ( FAILED( IDirectPlayX_DeletePlayerFromGroup( &lpCtxt->This->IDirectPlay4_iface,
                                                        lpCtxt->idGroup, dpId ) ) )
      ERR( "Unable to delete player 0x%08lx from grp 0x%08lx\n", dpId, lpCtxt->idGroup );

  return TRUE; /* Continue enumeration */
}

static HRESULT DP_IF_DestroyGroup( IDirectPlayImpl *This, void *lpMsgHdr, DPID idGroup, BOOL bAnsi )
{
  lpGroupData lpGData;
  DPRGOPContext context;

  FIXME( "(%p)->(%p,0x%08lx,%u): semi stub\n",
         This, lpMsgHdr, idGroup, bAnsi );

  /* Find the group */
  if( ( lpGData = DP_FindAnyGroup( This, idGroup ) ) == NULL )
  {
    return DPERR_INVALIDPLAYER; /* yes player */
  }

  context.This    = This;
  context.bAnsi   = bAnsi;
  context.idGroup = idGroup;

  /* Remove all players that this group has */
  IDirectPlayX_EnumGroupPlayers( &This->IDirectPlay4_iface, idGroup, NULL, cbRemoveGroupOrPlayer,
          &context, 0 );

  /* Remove all links to groups that this group has since this is dp3 */
  IDirectPlayX_EnumGroupsInGroup( &This->IDirectPlay4_iface, idGroup, NULL, cbRemoveGroupOrPlayer,
          (void*)&context, 0 );

  /* Remove this group from the parent group - if it has one */
  if( ( idGroup != DPID_SYSTEM_GROUP ) && ( lpGData->parent != DPID_SYSTEM_GROUP ) )
    IDirectPlayX_DeleteGroupFromGroup( &This->IDirectPlay4_iface, lpGData->parent, idGroup );

  /* Now delete this group data and list from the system group */
  DP_DeleteGroup( This, idGroup );

  /* Let the SP know that we've destroyed this group */
  if( This->dp2->spData.lpCB->DeleteGroup )
  {
    DPSP_DELETEGROUPDATA data;

    FIXME( "data.dwFlags is incorrect\n" );

    data.idGroup = idGroup;
    data.dwFlags = 0;
    data.lpISP   = This->dp2->spData.lpISP;

    (*This->dp2->spData.lpCB->DeleteGroup)( &data );
  }

  FIXME( "Send out a DESTORYPLAYERORGROUP message\n" );

  return DP_OK;
}

static HRESULT WINAPI IDirectPlay2AImpl_DestroyGroup( IDirectPlay2A *iface, DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_DestroyGroup( &This->IDirectPlay4A_iface, group );
}

static HRESULT WINAPI IDirectPlay2Impl_DestroyGroup( IDirectPlay2 *iface, DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_DestroyGroup( &This->IDirectPlay4_iface, group );
}

static HRESULT WINAPI IDirectPlay3AImpl_DestroyGroup( IDirectPlay3A *iface, DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_DestroyGroup( &This->IDirectPlay4_iface, group );
}

static HRESULT WINAPI IDirectPlay3Impl_DestroyGroup( IDirectPlay3 *iface, DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_DestroyGroup( &This->IDirectPlay4_iface, group );
}

static HRESULT WINAPI IDirectPlay4AImpl_DestroyGroup( IDirectPlay4A *iface, DPID idGroup )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return DP_IF_DestroyGroup( This, NULL, idGroup, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_DestroyGroup( IDirectPlay4 *iface, DPID idGroup )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    return DP_IF_DestroyGroup( This, NULL, idGroup, FALSE );
}

typedef struct _DPFAGContext
{
  IDirectPlayImpl   *This;
  DPID              idPlayer;
  BOOL              bAnsi;
} DPFAGContext, *lpDPFAGContext;

static HRESULT DP_IF_DestroyPlayer( IDirectPlayImpl *This, void *lpMsgHdr, DPID idPlayer,
        BOOL bAnsi )
{
  DPFAGContext cbContext;

  FIXME( "(%p)->(%p,0x%08lx,%u): semi stub\n",
         This, lpMsgHdr, idPlayer, bAnsi );

  if( This->dp2->connectionInitialized == NO_PROVIDER )
  {
    return DPERR_UNINITIALIZED;
  }

  if( DP_FindPlayer( This, idPlayer ) == NULL )
  {
    return DPERR_INVALIDPLAYER;
  }

  /* FIXME: If the player is remote, we must be the host to delete this */

  cbContext.This     = This;
  cbContext.idPlayer = idPlayer;
  cbContext.bAnsi    = bAnsi;

  /* Find each group and call DeletePlayerFromGroup if the player is a
     member of the group */
  IDirectPlayX_EnumGroups( &This->IDirectPlay4_iface, NULL, cbDeletePlayerFromAllGroups, &cbContext,
          DPENUMGROUPS_ALL );

  /* Now delete player and player list from the sys group */
  DP_DeletePlayer( This, idPlayer );

  /* Let the SP know that we've destroyed this group */
  if( This->dp2->spData.lpCB->DeletePlayer )
  {
    DPSP_DELETEPLAYERDATA data;

    FIXME( "data.dwFlags is incorrect\n" );

    data.idPlayer = idPlayer;
    data.dwFlags = 0;
    data.lpISP   = This->dp2->spData.lpISP;

    (*This->dp2->spData.lpCB->DeletePlayer)( &data );
  }

  FIXME( "Send a DELETEPLAYERORGROUP msg\n" );

  return DP_OK;
}

static BOOL CALLBACK
cbDeletePlayerFromAllGroups(
    DPID            dpId,
    DWORD           dwPlayerType,
    LPCDPNAME       lpName,
    DWORD           dwFlags,
    LPVOID          lpContext )
{
  lpDPFAGContext lpCtxt = (lpDPFAGContext)lpContext;

  if( dwPlayerType == DPPLAYERTYPE_GROUP )
  {
    IDirectPlayX_DeletePlayerFromGroup( &lpCtxt->This->IDirectPlay4_iface, dpId, lpCtxt->idPlayer );

    /* Enumerate all groups in this group since this will normally only
     * be called for top level groups
     */
    IDirectPlayX_EnumGroupsInGroup( &lpCtxt->This->IDirectPlay4_iface, dpId, NULL,
            cbDeletePlayerFromAllGroups, lpContext, DPENUMGROUPS_ALL);

  }
  else
  {
    ERR( "Group callback has dwPlayerType = 0x%08lx\n", dwPlayerType );
  }

  return TRUE;
}

static HRESULT WINAPI IDirectPlay2AImpl_DestroyPlayer( IDirectPlay2A *iface, DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_DestroyPlayer( &This->IDirectPlay4A_iface, player );
}

static HRESULT WINAPI IDirectPlay2Impl_DestroyPlayer( IDirectPlay2 *iface, DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_DestroyPlayer( &This->IDirectPlay4_iface, player );
}

static HRESULT WINAPI IDirectPlay3AImpl_DestroyPlayer( IDirectPlay3A *iface, DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_DestroyPlayer( &This->IDirectPlay4_iface, player );
}

static HRESULT WINAPI IDirectPlay3Impl_DestroyPlayer( IDirectPlay3 *iface, DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_DestroyPlayer( &This->IDirectPlay4_iface, player );
}

static HRESULT WINAPI IDirectPlay4AImpl_DestroyPlayer( IDirectPlay4A *iface, DPID idPlayer )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return DP_IF_DestroyPlayer( This, NULL, idPlayer, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_DestroyPlayer( IDirectPlay4 *iface, DPID idPlayer )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    return DP_IF_DestroyPlayer( This, NULL, idPlayer, FALSE );
}

static HRESULT WINAPI IDirectPlay2AImpl_EnumGroupPlayers( IDirectPlay2A *iface, DPID group,
        GUID *instance, LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_EnumGroupPlayers( &This->IDirectPlay4A_iface, group, instance,
            enumplayercb, context, flags );
}

static HRESULT WINAPI IDirectPlay2Impl_EnumGroupPlayers( IDirectPlay2 *iface, DPID group,
        GUID *instance, LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_EnumGroupPlayers( &This->IDirectPlay4_iface, group, instance,
            enumplayercb, context, flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_EnumGroupPlayers( IDirectPlay3A *iface, DPID group,
        GUID *instance, LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_EnumGroupPlayers( &This->IDirectPlay4_iface, group, instance,
            enumplayercb, context, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_EnumGroupPlayers( IDirectPlay3 *iface, DPID group,
        GUID *instance, LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_EnumGroupPlayers( &This->IDirectPlay4_iface, group, instance,
            enumplayercb, context, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_EnumGroupPlayers( IDirectPlay4A *iface, DPID group,
        GUID *instance, LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_EnumGroupPlayers( &This->IDirectPlay4_iface, group, instance, enumplayercb,
            context, flags );
}

static HRESULT WINAPI IDirectPlay4Impl_EnumGroupPlayers( IDirectPlay4 *iface, DPID group,
        GUID *instance, LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    lpGroupData  gdata;
    lpPlayerList plist;

    FIXME( "(%p)->(0x%08lx,%p,%p,%p,0x%08lx): semi stub\n", This, group, instance, enumplayercb,
           context, flags );

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;

    /* Find the group */
    if ( ( gdata = DP_FindAnyGroup( This, group ) ) == NULL )
        return DPERR_INVALIDGROUP;

    if ( DPQ_IS_EMPTY( gdata->players ) )
        return DP_OK;

    /* Walk the players in this group */
    for( plist = DPQ_FIRST( gdata->players ); ; plist = DPQ_NEXT( plist->players ) )
    {
        /* We do not enum the name server or app server as they are of no
         * consequence to the end user.
         */
        if ( ( plist->lpPData->dpid != DPID_NAME_SERVER ) &&
                ( plist->lpPData->dpid != DPID_SERVERPLAYER ) )
        {
            /* FIXME: Need to add stuff for flags checking */
            if ( !enumplayercb( plist->lpPData->dpid, DPPLAYERTYPE_PLAYER,
                        &plist->lpPData->name, plist->lpPData->dwFlags, context ) )
              /* User requested break */
              return DP_OK;
        }

        if ( DPQ_IS_ENDOFLIST( plist->players ) )
            break;
    }
    return DP_OK;
}

/* NOTE: This only enumerates top level groups (created with CreateGroup) */
static HRESULT WINAPI IDirectPlay2AImpl_EnumGroups( IDirectPlay2A *iface, GUID *instance,
        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_EnumGroups( &This->IDirectPlay4A_iface, instance, enumplayercb, context,
            flags );
}

static HRESULT WINAPI IDirectPlay2Impl_EnumGroups( IDirectPlay2 *iface, GUID *instance,
        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_EnumGroups( &This->IDirectPlay4_iface, instance, enumplayercb, context,
            flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_EnumGroups( IDirectPlay3A *iface, GUID *instance,
        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_EnumGroups( &This->IDirectPlay4_iface, instance, enumplayercb, context,
            flags );
}

static HRESULT WINAPI IDirectPlay3Impl_EnumGroups( IDirectPlay3 *iface, GUID *instance,
        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_EnumGroups( &This->IDirectPlay4_iface, instance, enumplayercb, context,
            flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_EnumGroups( IDirectPlay4A *iface, GUID *instance,
        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    return IDirectPlayX_EnumGroupsInGroup( iface, DPID_SYSTEM_GROUP, instance, enumplayercb,
            context, flags );
}

static HRESULT WINAPI IDirectPlay4Impl_EnumGroups ( IDirectPlay4 *iface, GUID *instance,
        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    return IDirectPlayX_EnumGroupsInGroup( iface, DPID_SYSTEM_GROUP, instance, enumplayercb,
            context, flags );
}

static HRESULT WINAPI IDirectPlay2AImpl_EnumPlayers( IDirectPlay2A *iface, GUID *instance,
        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_EnumPlayers( &This->IDirectPlay4A_iface, instance, enumplayercb, context,
            flags );
}

static HRESULT WINAPI IDirectPlay2Impl_EnumPlayers( IDirectPlay2 *iface, GUID *instance,
        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_EnumPlayers( &This->IDirectPlay4_iface, instance, enumplayercb, context,
            flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_EnumPlayers( IDirectPlay3A *iface, GUID *instance,
        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_EnumPlayers( &This->IDirectPlay4_iface, instance, enumplayercb, context,
            flags );
}

static HRESULT WINAPI IDirectPlay3Impl_EnumPlayers( IDirectPlay3 *iface, GUID *instance,
        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_EnumPlayers( &This->IDirectPlay4_iface, instance, enumplayercb, context,
            flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_EnumPlayers( IDirectPlay4A *iface, GUID *instance,
        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    return IDirectPlayX_EnumGroupPlayers( iface, DPID_SYSTEM_GROUP, instance, enumplayercb,
            context, flags );
}

static HRESULT WINAPI IDirectPlay4Impl_EnumPlayers( IDirectPlay4 *iface, GUID *instance,
        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    return IDirectPlayX_EnumGroupPlayers( iface, DPID_SYSTEM_GROUP, instance, enumplayercb,
            context, flags );
}

/* This function should call the registered callback function that the user
   passed into EnumSessions for each entry available.
 */
static void DP_InvokeEnumSessionCallbacks
       ( LPDPENUMSESSIONSCALLBACK2 lpEnumSessionsCallback2,
         LPVOID lpNSInfo,
         DWORD dwTimeout,
         LPVOID lpContext )
{
  LPDPSESSIONDESC2 lpSessionDesc;

  FIXME( ": not checking for conditions\n" );

  /* Not sure if this should be pruning but it's convenient */
  NS_PruneSessionCache( lpNSInfo );

  NS_ResetSessionEnumeration( lpNSInfo );

  /* Enumerate all sessions */
  /* FIXME: Need to indicate ANSI */
  while( (lpSessionDesc = NS_WalkSessions( lpNSInfo ) ) != NULL )
  {
    TRACE( "EnumSessionsCallback2 invoked\n" );
    if( !lpEnumSessionsCallback2( lpSessionDesc, &dwTimeout, 0, lpContext ) )
    {
      return;
    }
  }

  /* Invoke one last time to indicate that there is no more to come */
  lpEnumSessionsCallback2( NULL, &dwTimeout, DPESC_TIMEDOUT, lpContext );
}

static DWORD CALLBACK DP_EnumSessionsSendAsyncRequestThread( LPVOID lpContext )
{
  EnumSessionAsyncCallbackData* data = lpContext;
  HANDLE hSuicideRequest = data->hSuicideRequest;
  DWORD dwTimeout = data->dwTimeout;

  TRACE( "Thread started with timeout = 0x%08lx\n", dwTimeout );

  for( ;; )
  {
    HRESULT hr;

    /* Sleep up to dwTimeout waiting for request to terminate thread */
    if( WaitForSingleObject( hSuicideRequest, dwTimeout ) == WAIT_OBJECT_0 )
    {
      TRACE( "Thread terminating on terminate request\n" );
      break;
    }

    /* Now resend the enum request */
    hr = NS_SendSessionRequestBroadcast( &data->requestGuid,
                                         data->dwEnumSessionFlags,
                                         data->lpSpData );

    if( FAILED(hr) )
    {
      ERR( "Enum broadcase request failed: %s\n", DPLAYX_HresultToString(hr) );
      /* FIXME: Should we kill this thread? How to inform the main thread? */
    }

  }

  TRACE( "Thread terminating\n" );

  /* Clean up the thread data */
  CloseHandle( hSuicideRequest );
  free( lpContext );

  /* FIXME: Need to have some notification to main app thread that this is
   *        dead. It would serve two purposes. 1) allow sync on termination
   *        so that we don't actually send something to ourselves when we
   *        become name server (race condition) and 2) so that if we die
   *        abnormally something else will be able to tell.
   */

  return 1;
}

static void DP_KillEnumSessionThread( IDirectPlayImpl *This )
{
  /* Does a thread exist? If so we were doing an async enum session */
  if( This->dp2->hEnumSessionThread != INVALID_HANDLE_VALUE )
  {
    TRACE( "Killing EnumSession thread %p\n",
           This->dp2->hEnumSessionThread );

    /* Request that the thread kill itself nicely */
    SetEvent( This->dp2->hKillEnumSessionThreadEvent );
    CloseHandle( This->dp2->hKillEnumSessionThreadEvent );

    /* We no longer need to know about the thread */
    CloseHandle( This->dp2->hEnumSessionThread );

    This->dp2->hEnumSessionThread = INVALID_HANDLE_VALUE;
  }
}

static HRESULT WINAPI IDirectPlay2AImpl_EnumSessions( IDirectPlay2A *iface, DPSESSIONDESC2 *sdesc,
        DWORD timeout, LPDPENUMSESSIONSCALLBACK2 enumsessioncb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_EnumSessions( &This->IDirectPlay4A_iface, sdesc, timeout, enumsessioncb,
            context, flags );
}

static HRESULT WINAPI IDirectPlay2Impl_EnumSessions( IDirectPlay2 *iface, DPSESSIONDESC2 *sdesc,
        DWORD timeout, LPDPENUMSESSIONSCALLBACK2 enumsessioncb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_EnumSessions( &This->IDirectPlay4_iface, sdesc, timeout, enumsessioncb,
            context, flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_EnumSessions( IDirectPlay3A *iface, DPSESSIONDESC2 *sdesc,
        DWORD timeout, LPDPENUMSESSIONSCALLBACK2 enumsessioncb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_EnumSessions( &This->IDirectPlay4_iface, sdesc, timeout, enumsessioncb,
            context, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_EnumSessions( IDirectPlay3 *iface, DPSESSIONDESC2 *sdesc,
        DWORD timeout, LPDPENUMSESSIONSCALLBACK2 enumsessioncb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_EnumSessions( &This->IDirectPlay4_iface, sdesc, timeout, enumsessioncb,
            context, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_EnumSessions( IDirectPlay4A *iface, DPSESSIONDESC2 *sdesc,
        DWORD timeout, LPDPENUMSESSIONSCALLBACK2 enumsessioncb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_EnumSessions( &This->IDirectPlay4_iface, sdesc, timeout, enumsessioncb,
            context, flags );
}

static HRESULT WINAPI IDirectPlay4Impl_EnumSessions( IDirectPlay4 *iface, DPSESSIONDESC2 *sdesc,
        DWORD timeout, LPDPENUMSESSIONSCALLBACK2 enumsessioncb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    void *connection;
    DWORD  size;
    HRESULT hr = DP_OK;

    TRACE( "(%p)->(%p,0x%08lx,%p,%p,0x%08lx)\n", This, sdesc, timeout, enumsessioncb,
            context, flags );

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;

    /* Can't enumerate if the interface is already open */
    if ( This->dp2->bConnectionOpen )
        return DPERR_GENERIC;

    /* The loading of a lobby provider _seems_ to require a backdoor loading
     * of the service provider to also associate with this DP object. This is
     * because the app doesn't seem to have to call EnumConnections and
     * InitializeConnection for the SP before calling this method. As such
     * we'll do their dirty work for them with a quick hack so as to always
     * load the TCP/IP service provider.
     *
     * The correct solution would seem to involve creating a dialog box which
     * contains the possible SPs. These dialog boxes most likely follow SDK
     * examples.
     */
    if ( This->dp2->bDPLSPInitialized && !This->dp2->bSPInitialized )
    {
        WARN( "Hack providing TCP/IP SP for lobby provider activated\n" );

        if ( !DP_BuildSPCompoundAddr( (GUID*)&DPSPGUID_TCPIP, &connection, &size ) )
        {
            ERR( "Can't build compound addr\n" );
            return DPERR_GENERIC;
        }

        hr = IDirectPlayX_InitializeConnection( &This->IDirectPlay4_iface, connection, 0 );
        if ( FAILED(hr) )
            return hr;

        free( connection );
        This->dp2->bSPInitialized = TRUE;
    }


    /* Use the service provider default? */
    if ( !timeout )
    {
        DPCAPS caps;
        caps.dwSize = sizeof( caps );

        IDirectPlayX_GetCaps( &This->IDirectPlay4_iface, &caps, 0 );
        timeout = caps.dwTimeout;
        if ( !timeout )
            timeout = DPMSG_WAIT_5_SECS; /* Provide the TCP/IP default */
    }

    if ( flags & DPENUMSESSIONS_STOPASYNC )
    {
        DP_KillEnumSessionThread( This );
        return hr;
    }

    if ( flags & DPENUMSESSIONS_ASYNC )
    {
        /* Enumerate everything presently in the local session cache */
        DP_InvokeEnumSessionCallbacks( enumsessioncb, This->dp2->lpNameServerData, timeout,
                context );

        if ( This->dp2->dwEnumSessionLock )
            return DPERR_CONNECTING;

        /* See if we've already created a thread to service this interface */
        if ( This->dp2->hEnumSessionThread == INVALID_HANDLE_VALUE )
        {
            DWORD tid;
            This->dp2->dwEnumSessionLock++;

            /* Send the first enum request inline since the user may cancel a dialog
             * if one is presented. Also, may also have a connecting return code.
             */
            hr = NS_SendSessionRequestBroadcast( &sdesc->guidApplication, flags,
                    &This->dp2->spData );

            if ( SUCCEEDED(hr) )
            {
                EnumSessionAsyncCallbackData* data = calloc( 1, sizeof( *data ) );
                /* FIXME: need to kill the thread on object deletion */
                data->lpSpData  = &This->dp2->spData;
                data->requestGuid = sdesc->guidApplication;
                data->dwEnumSessionFlags = flags;
                data->dwTimeout = timeout;

                This->dp2->hKillEnumSessionThreadEvent = CreateEventW( NULL, TRUE, FALSE, NULL );
                if ( !DuplicateHandle( GetCurrentProcess(), This->dp2->hKillEnumSessionThreadEvent,
                            GetCurrentProcess(), &data->hSuicideRequest, 0, FALSE,
                            DUPLICATE_SAME_ACCESS ) )
                    ERR( "Can't duplicate thread killing handle\n" );

                TRACE( ": creating EnumSessionsRequest thread\n" );
                This->dp2->hEnumSessionThread = CreateThread( NULL, 0,
                        DP_EnumSessionsSendAsyncRequestThread, data, 0, &tid );
            }
            This->dp2->dwEnumSessionLock--;
        }
    }
    else
    {
        /* Invalidate the session cache for the interface */
        NS_InvalidateSessionCache( This->dp2->lpNameServerData );
        /* Send the broadcast for session enumeration */
        hr = NS_SendSessionRequestBroadcast( &sdesc->guidApplication, flags, &This->dp2->spData );
        SleepEx( timeout, FALSE );
        DP_InvokeEnumSessionCallbacks( enumsessioncb, This->dp2->lpNameServerData, timeout,
                context );
    }

    return hr;
}

static HRESULT WINAPI IDirectPlay2AImpl_GetCaps( IDirectPlay2A *iface, DPCAPS *caps, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_GetCaps( &This->IDirectPlay4A_iface, caps, flags );
}

static HRESULT WINAPI IDirectPlay2Impl_GetCaps( IDirectPlay2 *iface, DPCAPS *caps, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_GetCaps( &This->IDirectPlay4_iface, caps, flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_GetCaps( IDirectPlay3A *iface, DPCAPS *caps, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_GetCaps( &This->IDirectPlay4_iface, caps, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_GetCaps( IDirectPlay3 *iface, DPCAPS *caps, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_GetCaps( &This->IDirectPlay4_iface, caps, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_GetCaps( IDirectPlay4A *iface, DPCAPS *caps, DWORD flags )
{
    return IDirectPlayX_GetPlayerCaps( iface, DPID_ALLPLAYERS, caps, flags );
}

static HRESULT WINAPI IDirectPlay4Impl_GetCaps( IDirectPlay4 *iface, DPCAPS *caps, DWORD flags )
{
    return IDirectPlayX_GetPlayerCaps( iface, DPID_ALLPLAYERS, caps, flags );
}

static HRESULT WINAPI IDirectPlay2AImpl_GetGroupData( IDirectPlay2A *iface, DPID group, void *data,
        DWORD *size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_GetGroupData( &This->IDirectPlay4A_iface, group, data, size, flags );
}

static HRESULT WINAPI IDirectPlay2Impl_GetGroupData( IDirectPlay2 *iface, DPID group, void *data,
        DWORD *size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_GetGroupData( &This->IDirectPlay4_iface, group, data, size, flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_GetGroupData( IDirectPlay3A *iface, DPID group, void *data,
        DWORD *size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_GetGroupData( &This->IDirectPlay4_iface, group, data, size, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_GetGroupData( IDirectPlay3 *iface, DPID group, void *data,
        DWORD *size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_GetGroupData( &This->IDirectPlay4_iface, group, data, size, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_GetGroupData( IDirectPlay4A *iface, DPID group,
        void *data, DWORD *size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_GetGroupData( &This->IDirectPlay4_iface, group, data, size, flags );
}

static HRESULT WINAPI IDirectPlay4Impl_GetGroupData( IDirectPlay4 *iface, DPID group,
        void *data, DWORD *size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    lpGroupData gdata;
    DWORD bufsize;
    void *src;

    TRACE( "(%p)->(0x%08lx,%p,%p,0x%08lx)\n", This, group, data, size, flags );

    if ( ( gdata = DP_FindAnyGroup( This, group ) ) == NULL )
        return DPERR_INVALIDGROUP;

    /* How much buffer is required? */
    if ( flags & DPSET_LOCAL )
    {
        bufsize = gdata->dwLocalDataSize;
        src = gdata->lpLocalData;
    }
    else
    {
        bufsize = gdata->dwRemoteDataSize;
        src = gdata->lpRemoteData;
    }

    /* Is the user requesting to know how big a buffer is required? */
    if ( !data || *size < bufsize )
    {
        *size = bufsize;
        return DPERR_BUFFERTOOSMALL;
    }

    CopyMemory( data, src, bufsize );

    return DP_OK;
}

static HRESULT DP_IF_GetGroupName( IDirectPlayImpl *This, DPID idGroup, void *lpData,
        DWORD *lpdwDataSize, BOOL bAnsi )
{
  lpGroupData lpGData;
  LPDPNAME    lpName = lpData;
  DWORD       dwRequiredDataSize;

  FIXME("(%p)->(0x%08lx,%p,%p,%u) ANSI ignored\n",
          This, idGroup, lpData, lpdwDataSize, bAnsi );

  if( ( lpGData = DP_FindAnyGroup( This, idGroup ) ) == NULL )
  {
    return DPERR_INVALIDGROUP;
  }

  dwRequiredDataSize = lpGData->name.dwSize;

  if( lpGData->name.lpszShortNameA )
  {
    dwRequiredDataSize += strlen( lpGData->name.lpszShortNameA ) + 1;
  }

  if( lpGData->name.lpszLongNameA )
  {
    dwRequiredDataSize += strlen( lpGData->name.lpszLongNameA ) + 1;
  }

  if( ( lpData == NULL ) ||
      ( *lpdwDataSize < dwRequiredDataSize )
    )
  {
    *lpdwDataSize = dwRequiredDataSize;
    return DPERR_BUFFERTOOSMALL;
  }

  /* Copy the structure */
  CopyMemory( lpName, &lpGData->name, lpGData->name.dwSize );

  if( lpGData->name.lpszShortNameA )
  {
    strcpy( ((char*)lpName)+lpGData->name.dwSize,
            lpGData->name.lpszShortNameA );
  }
  else
  {
    lpName->lpszShortNameA = NULL;
  }

  if( lpGData->name.lpszShortNameA )
  {
    strcpy( ((char*)lpName)+lpGData->name.dwSize,
            lpGData->name.lpszShortNameA );
  }
  else
  {
    lpName->lpszLongNameA = NULL;
  }

  return DP_OK;
}

static HRESULT WINAPI IDirectPlay2AImpl_GetGroupName( IDirectPlay2A *iface, DPID group, void *data,
        DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_GetGroupName( &This->IDirectPlay4A_iface, group, data, size );
}

static HRESULT WINAPI IDirectPlay2Impl_GetGroupName( IDirectPlay2 *iface, DPID group, void *data,
        DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_GetGroupName( &This->IDirectPlay4_iface, group, data, size );
}

static HRESULT WINAPI IDirectPlay3AImpl_GetGroupName( IDirectPlay3A *iface, DPID group, void *data,
        DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_GetGroupName( &This->IDirectPlay4_iface, group, data, size );
}

static HRESULT WINAPI IDirectPlay3Impl_GetGroupName( IDirectPlay3 *iface, DPID group, void *data,
        DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_GetGroupName( &This->IDirectPlay4_iface, group, data, size );
}

static HRESULT WINAPI IDirectPlay4AImpl_GetGroupName( IDirectPlay4A *iface, DPID idGroup,
        void *lpData, DWORD *lpdwDataSize )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return DP_IF_GetGroupName( This, idGroup, lpData, lpdwDataSize, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_GetGroupName( IDirectPlay4 *iface, DPID idGroup,
        void *lpData, DWORD *lpdwDataSize )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    return DP_IF_GetGroupName( This, idGroup, lpData, lpdwDataSize, FALSE );
}

static HRESULT WINAPI IDirectPlay2AImpl_GetMessageCount( IDirectPlay2A *iface, DPID player,
        DWORD *count )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_GetMessageCount( &This->IDirectPlay4A_iface, player, count );
}

static HRESULT WINAPI IDirectPlay2Impl_GetMessageCount( IDirectPlay2 *iface, DPID player,
        DWORD *count )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_GetMessageCount( &This->IDirectPlay4_iface, player, count );
}

static HRESULT WINAPI IDirectPlay3AImpl_GetMessageCount( IDirectPlay3A *iface, DPID player,
        DWORD *count )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_GetMessageCount( &This->IDirectPlay4_iface, player, count );
}

static HRESULT WINAPI IDirectPlay3Impl_GetMessageCount( IDirectPlay3 *iface, DPID player,
        DWORD *count )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_GetMessageCount( &This->IDirectPlay4_iface, player, count );
}

static HRESULT WINAPI IDirectPlay4AImpl_GetMessageCount( IDirectPlay4A *iface, DPID player,
        DWORD *count )
{
    return IDirectPlayX_GetMessageQueue( iface, 0, player, DPMESSAGEQUEUE_RECEIVE, count, NULL );
}

static HRESULT WINAPI IDirectPlay4Impl_GetMessageCount( IDirectPlay4 *iface, DPID player,
        DWORD *count )
{
    return IDirectPlayX_GetMessageQueue( iface, 0, player, DPMESSAGEQUEUE_RECEIVE, count, NULL );
}

static HRESULT WINAPI IDirectPlay2AImpl_GetPlayerAddress( IDirectPlay2A *iface, DPID player,
        void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_GetPlayerAddress( &This->IDirectPlay4A_iface, player, data, size );
}

static HRESULT WINAPI IDirectPlay2Impl_GetPlayerAddress( IDirectPlay2 *iface, DPID player,
        void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_GetPlayerAddress( &This->IDirectPlay4_iface, player, data, size );
}

static HRESULT WINAPI IDirectPlay3AImpl_GetPlayerAddress( IDirectPlay3A *iface, DPID player,
        void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_GetPlayerAddress( &This->IDirectPlay4_iface, player, data, size );
}

static HRESULT WINAPI IDirectPlay3Impl_GetPlayerAddress( IDirectPlay3 *iface, DPID player,
        void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_GetPlayerAddress( &This->IDirectPlay4_iface, player, data, size );
}

static HRESULT WINAPI IDirectPlay4AImpl_GetPlayerAddress( IDirectPlay4A *iface, DPID player,
        void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    FIXME("(%p)->(0x%08lx,%p,%p): stub\n", This, player, data, size );
    return DP_OK;
}

static HRESULT WINAPI IDirectPlay4Impl_GetPlayerAddress( IDirectPlay4 *iface, DPID player,
        void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    FIXME( "(%p)->(0x%08lx,%p,%p): stub\n", This, player, data, size );
    return DP_OK;
}

static HRESULT WINAPI IDirectPlay2AImpl_GetPlayerCaps( IDirectPlay2A *iface, DPID player,
        DPCAPS *caps, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_GetPlayerCaps( &This->IDirectPlay4A_iface, player, caps, flags );
}

static HRESULT WINAPI IDirectPlay2Impl_GetPlayerCaps( IDirectPlay2 *iface, DPID player,
        DPCAPS *caps, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_GetPlayerCaps( &This->IDirectPlay4_iface, player, caps, flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_GetPlayerCaps( IDirectPlay3A *iface, DPID player,
        DPCAPS *caps, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_GetPlayerCaps( &This->IDirectPlay4_iface, player, caps, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_GetPlayerCaps( IDirectPlay3 *iface, DPID player,
        DPCAPS *caps, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_GetPlayerCaps( &This->IDirectPlay4_iface, player, caps, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_GetPlayerCaps( IDirectPlay4A *iface, DPID player,
        DPCAPS *caps, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_GetPlayerCaps( &This->IDirectPlay4_iface, player, caps, flags );
}

static HRESULT WINAPI IDirectPlay4Impl_GetPlayerCaps( IDirectPlay4 *iface, DPID player,
        DPCAPS *caps, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    DPSP_GETCAPSDATA data;

    TRACE( "(%p)->(0x%08lx,%p,0x%08lx)\n", This, player, caps, flags);

    if ( !caps )
        return DPERR_INVALIDPARAMS;

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;

    if( caps->dwSize != sizeof(DPCAPS) )
        return DPERR_INVALIDPARAMS;

    /* Query the service provider */
    data.idPlayer = player;
    data.dwFlags = flags;
    data.lpCaps = caps;
    data.lpISP = This->dp2->spData.lpISP;

    return (*This->dp2->spData.lpCB->GetCaps)( &data );
}

static HRESULT WINAPI IDirectPlay2AImpl_GetPlayerData( IDirectPlay2A *iface, DPID player,
        void *data, DWORD *size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_GetPlayerData( &This->IDirectPlay4A_iface, player, data, size, flags );
}

static HRESULT WINAPI IDirectPlay2Impl_GetPlayerData( IDirectPlay2 *iface, DPID player,
        void *data, DWORD *size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_GetPlayerData( &This->IDirectPlay4_iface, player, data, size, flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_GetPlayerData( IDirectPlay3A *iface, DPID player,
        void *data, DWORD *size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_GetPlayerData( &This->IDirectPlay4_iface, player, data, size, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_GetPlayerData( IDirectPlay3 *iface, DPID player,
        void *data, DWORD *size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_GetPlayerData( &This->IDirectPlay4_iface, player, data, size, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_GetPlayerData( IDirectPlay4A *iface, DPID player,
        void *data, DWORD *size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_GetPlayerData( &This->IDirectPlay4_iface, player, data, size, flags );
}

static HRESULT WINAPI IDirectPlay4Impl_GetPlayerData( IDirectPlay4 *iface, DPID player,
        void *data, DWORD *size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    lpPlayerList plist;
    DWORD bufsize;
    void *src;

    TRACE( "(%p)->(0x%08lx,%p,%p,0x%08lx)\n", This, player, data, size, flags );

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;

    if ( ( plist = DP_FindPlayer( This, player ) ) == NULL )
        return DPERR_INVALIDPLAYER;

    if ( flags & DPSET_LOCAL )
    {
        bufsize = plist->lpPData->dwLocalDataSize;
        src = plist->lpPData->lpLocalData;
    }
    else
    {
        bufsize = plist->lpPData->dwRemoteDataSize;
        src = plist->lpPData->lpRemoteData;
    }

    /* Is the user requesting to know how big a buffer is required? */
    if ( !data || *size < bufsize )
    {
        *size = bufsize;
        return DPERR_BUFFERTOOSMALL;
    }

    CopyMemory( data, src, bufsize );

    return DP_OK;
}

static HRESULT DP_IF_GetPlayerName( IDirectPlayImpl *This, DPID idPlayer, void *lpData,
        DWORD *lpdwDataSize, BOOL bAnsi )
{
  lpPlayerList lpPList;
  LPDPNAME    lpName = lpData;
  DWORD       dwRequiredDataSize;

  FIXME( "(%p)->(0x%08lx,%p,%p,%u): ANSI\n",
         This, idPlayer, lpData, lpdwDataSize, bAnsi );

  if( This->dp2->connectionInitialized == NO_PROVIDER )
  {
    return DPERR_UNINITIALIZED;
  }

  if( ( lpPList = DP_FindPlayer( This, idPlayer ) ) == NULL )
  {
    return DPERR_INVALIDPLAYER;
  }

  dwRequiredDataSize = lpPList->lpPData->name.dwSize;

  if( lpPList->lpPData->name.lpszShortNameA )
  {
    dwRequiredDataSize += strlen( lpPList->lpPData->name.lpszShortNameA ) + 1;
  }

  if( lpPList->lpPData->name.lpszLongNameA )
  {
    dwRequiredDataSize += strlen( lpPList->lpPData->name.lpszLongNameA ) + 1;
  }

  if( ( lpData == NULL ) ||
      ( *lpdwDataSize < dwRequiredDataSize )
    )
  {
    *lpdwDataSize = dwRequiredDataSize;
    return DPERR_BUFFERTOOSMALL;
  }

  /* Copy the structure */
  CopyMemory( lpName, &lpPList->lpPData->name, lpPList->lpPData->name.dwSize );

  if( lpPList->lpPData->name.lpszShortNameA )
  {
    strcpy( ((char*)lpName)+lpPList->lpPData->name.dwSize,
            lpPList->lpPData->name.lpszShortNameA );
  }
  else
  {
    lpName->lpszShortNameA = NULL;
  }

  if( lpPList->lpPData->name.lpszShortNameA )
  {
    strcpy( ((char*)lpName)+lpPList->lpPData->name.dwSize,
            lpPList->lpPData->name.lpszLongNameA );
  }
  else
  {
    lpName->lpszLongNameA = NULL;
  }

  return DP_OK;
}

static HRESULT WINAPI IDirectPlay2AImpl_GetPlayerName( IDirectPlay2A *iface, DPID player,
        void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_GetPlayerName( &This->IDirectPlay4A_iface, player, data, size );
}

static HRESULT WINAPI IDirectPlay2Impl_GetPlayerName( IDirectPlay2 *iface, DPID player,
        void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_GetPlayerName( &This->IDirectPlay4_iface, player, data, size );
}

static HRESULT WINAPI IDirectPlay3AImpl_GetPlayerName( IDirectPlay3A *iface, DPID player,
        void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_GetPlayerName( &This->IDirectPlay4_iface, player, data, size );
}

static HRESULT WINAPI IDirectPlay3Impl_GetPlayerName( IDirectPlay3 *iface, DPID player,
        void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_GetPlayerName( &This->IDirectPlay4_iface, player, data, size );
}

static HRESULT WINAPI IDirectPlay4AImpl_GetPlayerName( IDirectPlay4A *iface, DPID idPlayer,
        void *lpData, DWORD *lpdwDataSize )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return DP_IF_GetPlayerName( This, idPlayer, lpData, lpdwDataSize, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_GetPlayerName( IDirectPlay4 *iface, DPID idPlayer,
        void *lpData, DWORD *lpdwDataSize )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    return DP_IF_GetPlayerName( This, idPlayer, lpData, lpdwDataSize, FALSE );
}

static HRESULT DP_GetSessionDesc( IDirectPlayImpl *This, void *lpData, DWORD *lpdwDataSize,
        BOOL bAnsi )
{
  DWORD dwRequiredSize;

  TRACE( "(%p)->(%p,%p,%u)\n", This, lpData, lpdwDataSize, bAnsi );

  if( This->dp2->connectionInitialized == NO_PROVIDER )
  {
    return DPERR_UNINITIALIZED;
  }

  if( ( lpData == NULL ) && ( lpdwDataSize == NULL ) )
  {
    return DPERR_INVALIDPARAMS;
  }

  /* FIXME: Get from This->dp2->lpSessionDesc */
  dwRequiredSize = DP_CalcSessionDescSize( This->dp2->lpSessionDesc, bAnsi );

  if ( ( lpData == NULL ) ||
       ( *lpdwDataSize < dwRequiredSize )
     )
  {
    *lpdwDataSize = dwRequiredSize;
    return DPERR_BUFFERTOOSMALL;
  }

  DP_CopySessionDesc( lpData, This->dp2->lpSessionDesc, bAnsi );

  return DP_OK;
}

static HRESULT WINAPI IDirectPlay2AImpl_GetSessionDesc( IDirectPlay2A *iface, void *data,
        DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_GetSessionDesc( &This->IDirectPlay4A_iface, data, size );
}

static HRESULT WINAPI IDirectPlay2Impl_GetSessionDesc( IDirectPlay2 *iface, void *data,
        DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_GetSessionDesc( &This->IDirectPlay4_iface, data, size );
}

static HRESULT WINAPI IDirectPlay3AImpl_GetSessionDesc( IDirectPlay3A *iface, void *data,
        DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_GetSessionDesc( &This->IDirectPlay4_iface, data, size );
}

static HRESULT WINAPI IDirectPlay3Impl_GetSessionDesc( IDirectPlay3 *iface, void *data,
        DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_GetSessionDesc( &This->IDirectPlay4_iface, data, size );
}

static HRESULT WINAPI IDirectPlay4AImpl_GetSessionDesc( IDirectPlay4A *iface, void *lpData,
        DWORD *lpdwDataSize )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return DP_GetSessionDesc( This, lpData, lpdwDataSize, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_GetSessionDesc( IDirectPlay4 *iface, void *lpData,
        DWORD *lpdwDataSize )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    return DP_GetSessionDesc( This, lpData, lpdwDataSize, TRUE );
}

static HRESULT WINAPI IDirectPlay2AImpl_Initialize( IDirectPlay2A *iface, GUID *guid )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_Initialize( &This->IDirectPlay4A_iface, guid );
}

static HRESULT WINAPI IDirectPlay2Impl_Initialize( IDirectPlay2 *iface, GUID *guid )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_Initialize( &This->IDirectPlay4_iface, guid );
}

static HRESULT WINAPI IDirectPlay3AImpl_Initialize( IDirectPlay3A *iface, GUID *guid )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_Initialize( &This->IDirectPlay4_iface, guid );
}

static HRESULT WINAPI IDirectPlay3Impl_Initialize( IDirectPlay3 *iface, GUID *guid )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_Initialize( &This->IDirectPlay4_iface, guid );
}

/* Intended only for COM compatibility. Always returns an error. */
static HRESULT WINAPI IDirectPlay4AImpl_Initialize( IDirectPlay4A *iface, GUID *guid )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    TRACE("(%p)->(%p): no-op\n", This, guid );
    return DPERR_ALREADYINITIALIZED;
}

static HRESULT WINAPI IDirectPlay4Impl_Initialize( IDirectPlay4 *iface, GUID *guid )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    TRACE( "(%p)->(%p): no-op\n", This, guid );
    return DPERR_ALREADYINITIALIZED;
}


static HRESULT DP_SecureOpen( IDirectPlayImpl *This, const DPSESSIONDESC2 *lpsd, DWORD dwFlags,
        const DPSECURITYDESC *lpSecurity, const DPCREDENTIALS *lpCredentials, BOOL bAnsi )
{
  HRESULT hr = DP_OK;

  FIXME( "(%p)->(%p,0x%08lx,%p,%p): partial stub\n",
         This, lpsd, dwFlags, lpSecurity, lpCredentials );

  if( This->dp2->connectionInitialized == NO_PROVIDER )
  {
    return DPERR_UNINITIALIZED;
  }

  if( lpsd->dwSize != sizeof(DPSESSIONDESC2) )
  {
    TRACE( ": rejecting invalid dpsd size (%ld).\n", lpsd->dwSize );
    return DPERR_INVALIDPARAMS;
  }

  if( This->dp2->bConnectionOpen )
  {
    TRACE( ": rejecting already open connection.\n" );
    return DPERR_ALREADYINITIALIZED;
  }

  /* If we're enumerating, kill the thread */
  DP_KillEnumSessionThread( This );

  if( dwFlags & DPOPEN_CREATE )
  {
    /* Rightoo - this computer is the host and the local computer needs to be
       the name server so that others can join this session */
    NS_SetLocalComputerAsNameServer( lpsd, This->dp2->lpNameServerData );

    This->dp2->bHostInterface = TRUE;

    hr = DP_SetSessionDesc( This, lpsd, 0, TRUE, bAnsi );
    if( FAILED( hr ) )
    {
      ERR( "Unable to set session desc: %s\n", DPLAYX_HresultToString( hr ) );
      return hr;
    }
  }

  /* Invoke the conditional callback for the service provider */
  if( This->dp2->spData.lpCB->Open )
  {
    DPSP_OPENDATA data;

    FIXME( "Not all data fields are correct. Need new parameter\n" );

    data.bCreate           = (dwFlags & DPOPEN_CREATE ) != 0;
    data.lpSPMessageHeader = (dwFlags & DPOPEN_CREATE ) ? NULL
                                                        : NS_GetNSAddr( This->dp2->lpNameServerData );
    data.lpISP             = This->dp2->spData.lpISP;
    data.bReturnStatus     = (dwFlags & DPOPEN_RETURNSTATUS) != 0;
    data.dwOpenFlags       = dwFlags;
    data.dwSessionFlags    = This->dp2->lpSessionDesc->dwFlags;

    hr = (*This->dp2->spData.lpCB->Open)(&data);
    if( FAILED( hr ) )
    {
      ERR( "Unable to open session: %s\n", DPLAYX_HresultToString( hr ) );
      return hr;
    }
  }

  {
    /* Create the system group of which everything is a part of */
    DPID systemGroup = DPID_SYSTEM_GROUP;

    hr = DP_IF_CreateGroup( This, NULL, &systemGroup, NULL,
                            NULL, 0, 0, TRUE );

  }

  if( dwFlags & DPOPEN_JOIN )
  {
    DPID dpidServerId = DPID_UNKNOWN;

    /* Create the server player for this interface. This way we can receive
     * messages for this session.
     */
    /* FIXME: I suppose that we should be setting an event for a receive
     *        type of thing. That way the messaging thread could know to wake
     *        up. DPlay would then trigger the hEvent for the player the
     *        message is directed to.
     */
    hr = DP_IF_CreatePlayer( This, NULL, &dpidServerId, NULL, 0, NULL,
                             0,
                             DPPLAYER_SERVERPLAYER | DPPLAYER_LOCAL , bAnsi );

  }
  else if( dwFlags & DPOPEN_CREATE )
  {
    DPID dpidNameServerId = DPID_NAME_SERVER;

    hr = DP_IF_CreatePlayer( This, NULL, &dpidNameServerId, NULL, 0, NULL,
                             0, DPPLAYER_SERVERPLAYER, bAnsi );
  }

  if( FAILED(hr) )
  {
    ERR( "Couldn't create name server/system player: %s\n",
         DPLAYX_HresultToString(hr) );
  }

  return hr;
}

static HRESULT WINAPI IDirectPlay2AImpl_Open( IDirectPlay2A *iface, DPSESSIONDESC2 *sdesc,
        DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_Open( &This->IDirectPlay4A_iface, sdesc, flags );
}

static HRESULT WINAPI IDirectPlay2Impl_Open( IDirectPlay2 *iface, DPSESSIONDESC2 *sdesc,
        DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_Open( &This->IDirectPlay4_iface, sdesc, flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_Open( IDirectPlay3A *iface, DPSESSIONDESC2 *sdesc,
        DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_Open( &This->IDirectPlay4_iface, sdesc, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_Open( IDirectPlay3 *iface, DPSESSIONDESC2 *sdesc,
        DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_Open( &This->IDirectPlay4_iface, sdesc, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_Open( IDirectPlay4A *iface, DPSESSIONDESC2 *sdesc,
        DWORD flags )
{
    return IDirectPlayX_SecureOpen( iface, sdesc, flags, NULL, NULL );
}

static HRESULT WINAPI IDirectPlay4Impl_Open( IDirectPlay4 *iface, DPSESSIONDESC2 *sdesc,
        DWORD flags )
{
    return IDirectPlayX_SecureOpen( iface, sdesc, flags, NULL, NULL );
}

static HRESULT DP_IF_Receive( IDirectPlayImpl *This, DPID *lpidFrom, DPID *lpidTo, DWORD dwFlags,
        void *lpData, DWORD *lpdwDataSize, BOOL bAnsi )
{
  LPDPMSG lpMsg = NULL;

  FIXME( "(%p)->(%p,%p,0x%08lx,%p,%p,%u): stub\n",
         This, lpidFrom, lpidTo, dwFlags, lpData, lpdwDataSize, bAnsi );

  if( This->dp2->connectionInitialized == NO_PROVIDER )
  {
    return DPERR_UNINITIALIZED;
  }

  if( dwFlags == 0 )
  {
    dwFlags = DPRECEIVE_ALL;
  }

  /* If the lpData is NULL, we must be peeking the message */
  if(  ( lpData == NULL ) &&
      !( dwFlags & DPRECEIVE_PEEK )
    )
  {
    return DPERR_INVALIDPARAMS;
  }

  if( dwFlags & DPRECEIVE_ALL )
  {
    lpMsg = This->dp2->receiveMsgs.lpQHFirst;

    if( !( dwFlags & DPRECEIVE_PEEK ) )
    {
      FIXME( "Remove from queue\n" );
    }
  }
  else if( ( dwFlags & DPRECEIVE_TOPLAYER ) ||
           ( dwFlags & DPRECEIVE_FROMPLAYER )
         )
  {
    FIXME( "Find matching message 0x%08lx\n", dwFlags );
  }
  else
  {
    ERR( "Hmmm..dwFlags 0x%08lx\n", dwFlags );
  }

  if( lpMsg == NULL )
  {
    return DPERR_NOMESSAGES;
  }

  /* Copy into the provided buffer */
  if (lpData) CopyMemory( lpData, lpMsg->msg, *lpdwDataSize );

  return DP_OK;
}

static HRESULT WINAPI IDirectPlay2AImpl_Receive( IDirectPlay2A *iface, DPID *from, DPID *to,
        DWORD flags, void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_Receive( &This->IDirectPlay4A_iface, from, to, flags, data, size );
}

static HRESULT WINAPI IDirectPlay2Impl_Receive( IDirectPlay2 *iface, DPID *from, DPID *to,
        DWORD flags, void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_Receive( &This->IDirectPlay4_iface, from, to, flags, data, size );
}

static HRESULT WINAPI IDirectPlay3AImpl_Receive( IDirectPlay3A *iface, DPID *from, DPID *to,
        DWORD flags, void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_Receive( &This->IDirectPlay4_iface, from, to, flags, data, size );
}

static HRESULT WINAPI IDirectPlay3Impl_Receive( IDirectPlay3 *iface, DPID *from, DPID *to,
        DWORD flags, void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_Receive( &This->IDirectPlay4_iface, from, to, flags, data, size );
}

static HRESULT WINAPI IDirectPlay4AImpl_Receive( IDirectPlay4A *iface, DPID *lpidFrom,
        DPID *lpidTo, DWORD dwFlags, void *lpData, DWORD *lpdwDataSize )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return DP_IF_Receive( This, lpidFrom, lpidTo, dwFlags, lpData, lpdwDataSize, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_Receive( IDirectPlay4 *iface, DPID *lpidFrom,
        DPID *lpidTo, DWORD dwFlags, void *lpData, DWORD *lpdwDataSize )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    return DP_IF_Receive( This, lpidFrom, lpidTo, dwFlags, lpData, lpdwDataSize, FALSE );
}

static HRESULT WINAPI IDirectPlay2AImpl_Send( IDirectPlay2A *iface, DPID from, DPID to,
        DWORD flags, void *data, DWORD size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_Send( &This->IDirectPlay4A_iface, from, to, flags, data, size );
}

static HRESULT WINAPI IDirectPlay2Impl_Send( IDirectPlay2 *iface, DPID from, DPID to,
        DWORD flags, void *data, DWORD size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_Send( &This->IDirectPlay4_iface, from, to, flags, data, size );
}

static HRESULT WINAPI IDirectPlay3AImpl_Send( IDirectPlay3A *iface, DPID from, DPID to,
        DWORD flags, void *data, DWORD size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_Send( &This->IDirectPlay4_iface, from, to, flags, data, size );
}

static HRESULT WINAPI IDirectPlay3Impl_Send( IDirectPlay3 *iface, DPID from, DPID to,
        DWORD flags, void *data, DWORD size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_Send( &This->IDirectPlay4_iface, from, to, flags, data, size );
}

static HRESULT WINAPI IDirectPlay4AImpl_Send( IDirectPlay4A *iface, DPID from, DPID to,
        DWORD flags, void *data, DWORD size )
{
    return IDirectPlayX_SendEx( iface, from, to, flags, data, size, 0, 0, NULL, NULL );
}

static HRESULT WINAPI IDirectPlay4Impl_Send( IDirectPlay4 *iface, DPID from, DPID to,
        DWORD flags, void *data, DWORD size )
{
    return IDirectPlayX_SendEx( iface, from, to, flags, data, size, 0, 0, NULL, NULL );
}

static HRESULT WINAPI IDirectPlay2AImpl_SetGroupData( IDirectPlay2A *iface, DPID group, void *data,
        DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_SetGroupData( &This->IDirectPlay4A_iface, group, data, size, flags );
}

static HRESULT WINAPI IDirectPlay2Impl_SetGroupData( IDirectPlay2 *iface, DPID group, void *data,
        DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_SetGroupData( &This->IDirectPlay4_iface, group, data, size, flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_SetGroupData( IDirectPlay3A *iface, DPID group, void *data,
        DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_SetGroupData( &This->IDirectPlay4_iface, group, data, size, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_SetGroupData( IDirectPlay3 *iface, DPID group, void *data,
        DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_SetGroupData( &This->IDirectPlay4_iface, group, data, size, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_SetGroupData( IDirectPlay4A *iface, DPID group, void *data,
        DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_SetGroupData( &This->IDirectPlay4_iface, group, data, size, flags );
}

static HRESULT WINAPI IDirectPlay4Impl_SetGroupData( IDirectPlay4 *iface, DPID group, void *data,
        DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    lpGroupData gdata;

    TRACE( "(%p)->(0x%08lx,%p,0x%08lx,0x%08lx)\n", This, group, data, size, flags );

    /* Parameter check */
    if ( !data && size )
        return DPERR_INVALIDPARAMS;

    /* Find the pointer to the data for this player */
    if ( ( gdata = DP_FindAnyGroup( This, group ) ) == NULL )
        return DPERR_INVALIDOBJECT;

    if ( !(flags & DPSET_LOCAL) )
    {
        FIXME( "Was this group created by this interface?\n" );
        /* FIXME: If this is a remote update need to allow it but not
         *        send a message.
         */
    }

    DP_SetGroupData( gdata, flags, data, size );

    /* FIXME: Only send a message if this group is local to the session otherwise
     * it will have been rejected above
     */
    if ( !(flags & DPSET_LOCAL) )
        FIXME( "Send msg?\n" );

    return DP_OK;
}

static HRESULT DP_IF_SetGroupName( IDirectPlayImpl *This, DPID idGroup, DPNAME *lpGroupName,
        DWORD dwFlags, BOOL bAnsi )
{
  lpGroupData lpGData;

  TRACE( "(%p)->(0x%08lx,%p,0x%08lx,%u)\n", This, idGroup,
         lpGroupName, dwFlags, bAnsi );

  if( ( lpGData = DP_FindAnyGroup( This, idGroup ) ) == NULL )
  {
    return DPERR_INVALIDGROUP;
  }

  DP_CopyDPNAMEStruct( &lpGData->name, lpGroupName, bAnsi );

  /* Should send a DPMSG_SETPLAYERORGROUPNAME message */
  FIXME( "Message not sent and dwFlags ignored\n" );

  return DP_OK;
}

static HRESULT WINAPI IDirectPlay2AImpl_SetGroupName( IDirectPlay2A *iface, DPID group,
        DPNAME *name, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_SetGroupName( &This->IDirectPlay4A_iface, group, name, flags );
}

static HRESULT WINAPI IDirectPlay2Impl_SetGroupName( IDirectPlay2 *iface, DPID group,
        DPNAME *name, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_SetGroupName( &This->IDirectPlay4_iface, group, name, flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_SetGroupName( IDirectPlay3A *iface, DPID group,
        DPNAME *name, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_SetGroupName( &This->IDirectPlay4_iface, group, name, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_SetGroupName( IDirectPlay3 *iface, DPID group,
        DPNAME *name, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_SetGroupName( &This->IDirectPlay4_iface, group, name, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_SetGroupName( IDirectPlay4A *iface, DPID idGroup,
        DPNAME *lpGroupName, DWORD dwFlags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return DP_IF_SetGroupName( This, idGroup, lpGroupName, dwFlags, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_SetGroupName( IDirectPlay4 *iface, DPID idGroup,
        DPNAME *lpGroupName, DWORD dwFlags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    return DP_IF_SetGroupName( This, idGroup, lpGroupName, dwFlags, FALSE );
}

static HRESULT WINAPI IDirectPlay2AImpl_SetPlayerData( IDirectPlay2A *iface, DPID player,
        void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_SetPlayerData( &This->IDirectPlay4A_iface, player, data, size, flags );
}

static HRESULT WINAPI IDirectPlay2Impl_SetPlayerData( IDirectPlay2 *iface, DPID player,
        void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_SetPlayerData( &This->IDirectPlay4_iface, player, data, size, flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_SetPlayerData( IDirectPlay3A *iface, DPID player,
        void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_SetPlayerData( &This->IDirectPlay4_iface, player, data, size, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_SetPlayerData( IDirectPlay3 *iface, DPID player,
        void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_SetPlayerData( &This->IDirectPlay4_iface, player, data, size, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_SetPlayerData( IDirectPlay4A *iface, DPID player,
        void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_SetPlayerData( &This->IDirectPlay4_iface, player, data, size, flags );
}

static HRESULT WINAPI IDirectPlay4Impl_SetPlayerData( IDirectPlay4 *iface, DPID player,
        void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    lpPlayerList plist;

    TRACE( "(%p)->(0x%08lx,%p,0x%08lx,0x%08lx)\n", This, player, data, size, flags );

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;

    /* Parameter check */
    if ( !data && size )
        return DPERR_INVALIDPARAMS;

    /* Find the pointer to the data for this player */
    if ( (plist = DP_FindPlayer( This, player )) == NULL )
        return DPERR_INVALIDPLAYER;

    if ( !(flags & DPSET_LOCAL) )
    {
        FIXME( "Was this group created by this interface?\n" );
        /* FIXME: If this is a remote update need to allow it but not
         *        send a message.
         */
    }

    DP_SetPlayerData( plist->lpPData, flags, data, size );

    if ( !(flags & DPSET_LOCAL) )
        FIXME( "Send msg?\n" );

    return DP_OK;
}

static HRESULT DP_IF_SetPlayerName( IDirectPlayImpl *This, DPID idPlayer, DPNAME *lpPlayerName,
        DWORD dwFlags, BOOL bAnsi )
{
  lpPlayerList lpPList;

  TRACE( "(%p)->(0x%08lx,%p,0x%08lx,%u)\n",
         This, idPlayer, lpPlayerName, dwFlags, bAnsi );

  if( This->dp2->connectionInitialized == NO_PROVIDER )
  {
    return DPERR_UNINITIALIZED;
  }

  if( ( lpPList = DP_FindPlayer( This, idPlayer ) ) == NULL )
  {
    return DPERR_INVALIDGROUP;
  }

  DP_CopyDPNAMEStruct( &lpPList->lpPData->name, lpPlayerName, bAnsi );

  /* Should send a DPMSG_SETPLAYERORGROUPNAME message */
  FIXME( "Message not sent and dwFlags ignored\n" );

  return DP_OK;
}

static HRESULT WINAPI IDirectPlay2AImpl_SetPlayerName( IDirectPlay2A *iface, DPID player,
        DPNAME *name, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_SetPlayerName( &This->IDirectPlay4A_iface, player, name, flags );
}

static HRESULT WINAPI IDirectPlay2Impl_SetPlayerName( IDirectPlay2 *iface, DPID player,
        DPNAME *name, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_SetPlayerName( &This->IDirectPlay4_iface, player, name, flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_SetPlayerName( IDirectPlay3A *iface, DPID player,
        DPNAME *name, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_SetPlayerName( &This->IDirectPlay4_iface, player, name, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_SetPlayerName( IDirectPlay3 *iface, DPID player,
        DPNAME *name, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_SetPlayerName( &This->IDirectPlay4_iface, player, name, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_SetPlayerName( IDirectPlay4A *iface, DPID idPlayer,
        DPNAME *lpPlayerName, DWORD dwFlags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return DP_IF_SetPlayerName( This, idPlayer, lpPlayerName, dwFlags, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_SetPlayerName( IDirectPlay4 *iface, DPID idPlayer,
        DPNAME *lpPlayerName, DWORD dwFlags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    return DP_IF_SetPlayerName( This, idPlayer, lpPlayerName, dwFlags, FALSE );
}

static HRESULT DP_SetSessionDesc( IDirectPlayImpl *This, const DPSESSIONDESC2 *lpSessDesc,
        DWORD dwFlags, BOOL bInitial, BOOL bAnsi  )
{
  DWORD            dwRequiredSize;
  LPDPSESSIONDESC2 lpTempSessDesc;

  TRACE( "(%p)->(%p,0x%08lx,%u,%u)\n",
         This, lpSessDesc, dwFlags, bInitial, bAnsi );

  if( This->dp2->connectionInitialized == NO_PROVIDER )
  {
    return DPERR_UNINITIALIZED;
  }

  if( dwFlags )
  {
    return DPERR_INVALIDPARAMS;
  }

  /* Only the host is allowed to update the session desc */
  if( !This->dp2->bHostInterface )
  {
    return DPERR_ACCESSDENIED;
  }

  /* FIXME: Copy into This->dp2->lpSessionDesc */
  dwRequiredSize = DP_CalcSessionDescSize( lpSessDesc, bAnsi );
  lpTempSessDesc = calloc( 1, dwRequiredSize );

  if( lpTempSessDesc == NULL )
  {
    return DPERR_OUTOFMEMORY;
  }

  /* Free the old */
  free( This->dp2->lpSessionDesc );

  This->dp2->lpSessionDesc = lpTempSessDesc;
  /* Set the new */
  DP_CopySessionDesc( This->dp2->lpSessionDesc, lpSessDesc, bAnsi );
  if( bInitial )
  {
    /*Initializing session GUID*/
    CoCreateGuid( &(This->dp2->lpSessionDesc->guidInstance) );
  }
  /* If this is an external invocation of the interface, we should be
   * letting everyone know that things have changed. Otherwise this is
   * just an initialization and it doesn't need to be propagated.
   */
  if( !bInitial )
  {
    FIXME( "Need to send a DPMSG_SETSESSIONDESC msg to everyone\n" );
  }

  return DP_OK;
}

static HRESULT WINAPI IDirectPlay2AImpl_SetSessionDesc( IDirectPlay2A *iface, DPSESSIONDESC2 *sdesc,
        DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_SetSessionDesc( &This->IDirectPlay4A_iface, sdesc, flags );
}

static HRESULT WINAPI IDirectPlay2Impl_SetSessionDesc( IDirectPlay2 *iface, DPSESSIONDESC2 *sdesc,
        DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_SetSessionDesc( &This->IDirectPlay4_iface, sdesc, flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_SetSessionDesc( IDirectPlay3A *iface, DPSESSIONDESC2 *sdesc,
        DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_SetSessionDesc( &This->IDirectPlay4_iface, sdesc, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_SetSessionDesc( IDirectPlay3 *iface, DPSESSIONDESC2 *sdesc,
        DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_SetSessionDesc( &This->IDirectPlay4_iface, sdesc, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_SetSessionDesc( IDirectPlay4A *iface,
        DPSESSIONDESC2 *lpSessDesc, DWORD dwFlags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return DP_SetSessionDesc( This, lpSessDesc, dwFlags, FALSE, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_SetSessionDesc( IDirectPlay4 *iface,
        DPSESSIONDESC2 *lpSessDesc, DWORD dwFlags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    return DP_SetSessionDesc( This, lpSessDesc, dwFlags, FALSE, TRUE );
}

/* FIXME: See about merging some of this stuff with dplayx_global.c stuff */
static DWORD DP_CalcSessionDescSize( LPCDPSESSIONDESC2 lpSessDesc, BOOL bAnsi )
{
  DWORD dwSize = 0;

  if( lpSessDesc == NULL )
  {
    /* Hmmm..don't need any size? */
    ERR( "NULL lpSessDesc\n" );
    return dwSize;
  }

  dwSize += sizeof( *lpSessDesc );

  if( bAnsi )
  {
    if( lpSessDesc->lpszSessionNameA )
    {
      dwSize += lstrlenA( lpSessDesc->lpszSessionNameA ) + 1;
    }

    if( lpSessDesc->lpszPasswordA )
    {
      dwSize += lstrlenA( lpSessDesc->lpszPasswordA ) + 1;
    }
  }
  else /* UNICODE */
  {
    if( lpSessDesc->lpszSessionName )
    {
      dwSize += sizeof( WCHAR ) *
        ( lstrlenW( lpSessDesc->lpszSessionName ) + 1 );
    }

    if( lpSessDesc->lpszPassword )
    {
      dwSize += sizeof( WCHAR ) *
        ( lstrlenW( lpSessDesc->lpszPassword ) + 1 );
    }
  }

  return dwSize;
}

/* Assumes that contiguous buffers are already allocated. */
static void DP_CopySessionDesc( LPDPSESSIONDESC2 lpSessionDest,
                                LPCDPSESSIONDESC2 lpSessionSrc, BOOL bAnsi )
{
  BYTE* lpStartOfFreeSpace;

  if( lpSessionDest == NULL )
  {
    ERR( "NULL lpSessionDest\n" );
    return;
  }

  CopyMemory( lpSessionDest, lpSessionSrc, sizeof( *lpSessionSrc ) );

  lpStartOfFreeSpace = ((BYTE*)lpSessionDest) + sizeof( *lpSessionSrc );

  if( bAnsi )
  {
    if( lpSessionSrc->lpszSessionNameA )
    {
      lstrcpyA( (LPSTR)lpStartOfFreeSpace,
                lpSessionDest->lpszSessionNameA );
      lpSessionDest->lpszSessionNameA = (LPSTR)lpStartOfFreeSpace;
      lpStartOfFreeSpace +=
        lstrlenA( lpSessionDest->lpszSessionNameA ) + 1;
    }

    if( lpSessionSrc->lpszPasswordA )
    {
      lstrcpyA( (LPSTR)lpStartOfFreeSpace,
                lpSessionDest->lpszPasswordA );
      lpSessionDest->lpszPasswordA = (LPSTR)lpStartOfFreeSpace;
    }
  }
  else /* UNICODE */
  {
    if( lpSessionSrc->lpszSessionName )
    {
      lstrcpyW( (LPWSTR)lpStartOfFreeSpace,
                lpSessionDest->lpszSessionName );
      lpSessionDest->lpszSessionName = (LPWSTR)lpStartOfFreeSpace;
      lpStartOfFreeSpace += sizeof(WCHAR) *
        ( lstrlenW( lpSessionDest->lpszSessionName ) + 1 );
    }

    if( lpSessionSrc->lpszPassword )
    {
      lstrcpyW( (LPWSTR)lpStartOfFreeSpace,
                lpSessionDest->lpszPassword );
      lpSessionDest->lpszPassword = (LPWSTR)lpStartOfFreeSpace;
    }
  }
}

static HRESULT WINAPI IDirectPlay3AImpl_AddGroupToGroup( IDirectPlay3A *iface, DPID parent,
        DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_AddGroupToGroup( &This->IDirectPlay4A_iface, parent, group );
}

static HRESULT WINAPI IDirectPlay3Impl_AddGroupToGroup( IDirectPlay3 *iface, DPID parent,
        DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_AddGroupToGroup( &This->IDirectPlay4_iface, parent, group );
}

static HRESULT WINAPI IDirectPlay4AImpl_AddGroupToGroup( IDirectPlay4A *iface, DPID parent,
        DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_AddGroupToGroup( &This->IDirectPlay4_iface, parent, group );
}

static HRESULT WINAPI IDirectPlay4Impl_AddGroupToGroup( IDirectPlay4 *iface, DPID parent,
        DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    lpGroupData gdata;
    lpGroupList glist;

    TRACE( "(%p)->(0x%08lx,0x%08lx)\n", This, parent, group );

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;

    if ( !DP_FindAnyGroup(This, parent ) )
        return DPERR_INVALIDGROUP;

    if ( ( gdata = DP_FindAnyGroup(This, group ) ) == NULL )
        return DPERR_INVALIDGROUP;

    /* Create a player list (ie "shortcut" ) */
    glist = calloc( 1, sizeof( *glist ) );
    if ( !glist )
        return DPERR_CANTADDPLAYER;

    /* Add the shortcut */
    gdata->uRef++;
    glist->lpGData = gdata;

    /* Add the player to the list of players for this group */
    DPQ_INSERT( gdata->groups, glist, groups );

    /* Send a ADDGROUPTOGROUP message */
    FIXME( "Not sending message\n" );

    return DP_OK;
}

static HRESULT DP_IF_CreateGroupInGroup( IDirectPlayImpl *This, void *lpMsgHdr, DPID idParentGroup,
        DPID *lpidGroup, DPNAME *lpGroupName, void *lpData, DWORD dwDataSize, DWORD dwFlags,
        BOOL bAnsi )
{
  lpGroupData lpGParentData;
  lpGroupList lpGList;
  lpGroupData lpGData;

  TRACE( "(%p)->(0x%08lx,%p,%p,%p,0x%08lx,0x%08lx,%u)\n",
         This, idParentGroup, lpidGroup, lpGroupName, lpData,
         dwDataSize, dwFlags, bAnsi );

  if( This->dp2->connectionInitialized == NO_PROVIDER )
  {
    return DPERR_UNINITIALIZED;
  }

  /* Verify that the specified parent is valid */
  if( ( lpGParentData = DP_FindAnyGroup(This, idParentGroup ) ) == NULL )
    return DPERR_INVALIDGROUP;

  lpGData = DP_CreateGroup(This, lpidGroup, lpGroupName, dwFlags, idParentGroup, bAnsi );

  if( lpGData == NULL )
  {
    return DPERR_CANTADDPLAYER; /* yes player not group */
  }

  /* Something else is referencing this data */
  lpGData->uRef++;

  DP_SetGroupData( lpGData, DPSET_REMOTE, lpData, dwDataSize );

  /* The list has now been inserted into the interface group list. We now
     need to put a "shortcut" to this group in the parent group */
  lpGList = calloc( 1, sizeof( *lpGList ) );
  if( lpGList == NULL )
  {
    FIXME( "Memory leak\n" );
    return DPERR_CANTADDPLAYER; /* yes player not group */
  }

  lpGList->lpGData = lpGData;

  DPQ_INSERT( lpGParentData->groups, lpGList, groups );

  /* Let the SP know that we've created this group */
  if( This->dp2->spData.lpCB->CreateGroup )
  {
    DPSP_CREATEGROUPDATA data;

    TRACE( "Calling SP CreateGroup\n" );

    data.idGroup           = *lpidGroup;
    data.dwFlags           = dwFlags;
    data.lpSPMessageHeader = lpMsgHdr;
    data.lpISP             = This->dp2->spData.lpISP;

    (*This->dp2->spData.lpCB->CreateGroup)( &data );
  }

  /* Inform all other peers of the creation of a new group. If there are
   * no peers keep this quiet.
   */
  if( This->dp2->lpSessionDesc &&
      ( This->dp2->lpSessionDesc->dwFlags & DPSESSION_MULTICASTSERVER ) )
  {
    DPMSG_CREATEPLAYERORGROUP msg;

    msg.dwType = DPSYS_CREATEPLAYERORGROUP;
    msg.dwPlayerType = DPPLAYERTYPE_GROUP;
    msg.dpId = *lpidGroup;
    msg.dwCurrentPlayers = idParentGroup; /* FIXME: Incorrect? */
    msg.lpData = lpData;
    msg.dwDataSize = dwDataSize;
    msg.dpnName = *lpGroupName;

    /* FIXME: Correct to just use send effectively? */
    /* FIXME: Should size include data w/ message or just message "header" */
    /* FIXME: Check return code */
    IDirectPlayX_SendEx( &This->IDirectPlay4_iface, DPID_SERVERPLAYER, DPID_ALLPLAYERS, 0, &msg,
            sizeof( msg ), 0, 0, NULL, NULL );
  }

  return DP_OK;
}

static HRESULT WINAPI IDirectPlay3AImpl_CreateGroupInGroup( IDirectPlay3A *iface, DPID parent,
        DPID *group, DPNAME *name, void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_CreateGroupInGroup( &This->IDirectPlay4A_iface, parent, group, name,
            data, size, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_CreateGroupInGroup( IDirectPlay3 *iface, DPID parent,
        DPID *group, DPNAME *name, void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_CreateGroupInGroup( &This->IDirectPlay4_iface, parent, group, name,
            data, size, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_CreateGroupInGroup( IDirectPlay4A *iface,
        DPID idParentGroup, DPID *lpidGroup, DPNAME *lpGroupName, void *lpData, DWORD dwDataSize,
        DWORD dwFlags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );

    *lpidGroup = DPID_UNKNOWN;

    return DP_IF_CreateGroupInGroup( This, NULL, idParentGroup, lpidGroup, lpGroupName, lpData,
            dwDataSize, dwFlags, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_CreateGroupInGroup( IDirectPlay4 *iface, DPID idParentGroup,
        DPID *lpidGroup, DPNAME *lpGroupName, void *lpData, DWORD dwDataSize, DWORD dwFlags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );

    *lpidGroup = DPID_UNKNOWN;

    return DP_IF_CreateGroupInGroup( This, NULL, idParentGroup, lpidGroup, lpGroupName, lpData,
            dwDataSize, dwFlags, FALSE );
}

static HRESULT WINAPI IDirectPlay3AImpl_DeleteGroupFromGroup( IDirectPlay3A *iface, DPID parent,
        DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_DeleteGroupFromGroup( &This->IDirectPlay4A_iface, parent, group );
}

static HRESULT WINAPI IDirectPlay3Impl_DeleteGroupFromGroup( IDirectPlay3 *iface, DPID parent,
        DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_DeleteGroupFromGroup( &This->IDirectPlay4_iface, parent, group );
}

static HRESULT WINAPI IDirectPlay4AImpl_DeleteGroupFromGroup( IDirectPlay4A *iface, DPID parent,
        DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_DeleteGroupFromGroup( &This->IDirectPlay4_iface, parent, group );
}

static HRESULT WINAPI IDirectPlay4Impl_DeleteGroupFromGroup( IDirectPlay4 *iface, DPID parent,
        DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    lpGroupList glist;
    lpGroupData parentdata;

    TRACE("(%p)->(0x%08lx,0x%08lx)\n", This, parent, group );

    /* Is the parent group valid? */
    if ( ( parentdata = DP_FindAnyGroup(This, parent ) ) == NULL )
        return DPERR_INVALIDGROUP;

    /* Remove the group from the parent group queue */
    DPQ_REMOVE_ENTRY( parentdata->groups, groups, lpGData->dpid, ==, group, glist );

    if ( glist == NULL )
        return DPERR_INVALIDGROUP;

    /* Decrement the ref count */
    glist->lpGData->uRef--;

    /* Free up the list item */
    free( glist );

    /* Should send a DELETEGROUPFROMGROUP message */
    FIXME( "message not sent\n" );

    return DP_OK;
}

static BOOL DP_BuildSPCompoundAddr( LPGUID lpcSpGuid, LPVOID* lplpAddrBuf,
                                    LPDWORD lpdwBufSize )
{
  DPCOMPOUNDADDRESSELEMENT dpCompoundAddress;
  HRESULT                  hr;

  dpCompoundAddress.dwDataSize = sizeof( GUID );
  dpCompoundAddress.guidDataType = DPAID_ServiceProvider;
  dpCompoundAddress.lpData = lpcSpGuid;

  *lplpAddrBuf = NULL;
  *lpdwBufSize = 0;

  hr = DPL_CreateCompoundAddress( &dpCompoundAddress, 1, *lplpAddrBuf,
                                  lpdwBufSize, TRUE );

  if( hr != DPERR_BUFFERTOOSMALL )
  {
    ERR( "can't get buffer size: %s\n", DPLAYX_HresultToString( hr ) );
    return FALSE;
  }

  /* Now allocate the buffer */
  *lplpAddrBuf = calloc( 1, *lpdwBufSize );

  hr = DPL_CreateCompoundAddress( &dpCompoundAddress, 1, *lplpAddrBuf,
                                  lpdwBufSize, TRUE );
  if( FAILED(hr) )
  {
    ERR( "can't create address: %s\n", DPLAYX_HresultToString( hr ) );
    return FALSE;
  }

  return TRUE;
}

static HRESULT WINAPI IDirectPlay3AImpl_EnumConnections( IDirectPlay3A *iface,
        const GUID *application, LPDPENUMCONNECTIONSCALLBACK enumcb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_EnumConnections( &This->IDirectPlay4A_iface, application, enumcb, context,
            flags );
}

static HRESULT WINAPI IDirectPlay3Impl_EnumConnections( IDirectPlay3 *iface,
        const GUID *application, LPDPENUMCONNECTIONSCALLBACK enumcb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_EnumConnections( &This->IDirectPlay4_iface, application, enumcb, context,
            flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_EnumConnections( IDirectPlay4A *iface,
        const GUID *lpguidApplication, LPDPENUMCONNECTIONSCALLBACK lpEnumCallback, void *lpContext,
        DWORD dwFlags )
{
  IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
  TRACE("(%p)->(%p,%p,%p,0x%08lx)\n", This, lpguidApplication, lpEnumCallback, lpContext, dwFlags );

  /* A default dwFlags (0) is backwards compatible -- DPCONNECTION_DIRECTPLAY */
  if( dwFlags == 0 )
  {
    dwFlags = DPCONNECTION_DIRECTPLAY;
  }

  if( ! ( ( dwFlags & DPCONNECTION_DIRECTPLAY ) ||
          ( dwFlags & DPCONNECTION_DIRECTPLAYLOBBY ) )
    )
  {
    return DPERR_INVALIDFLAGS;
  }

  if( !lpEnumCallback )
  {
     return DPERR_INVALIDPARAMS;
  }

  /* Enumerate DirectPlay service providers */
  if( dwFlags & DPCONNECTION_DIRECTPLAY )
  {
    HKEY hkResult;
    LPCSTR searchSubKey    = "SOFTWARE\\Microsoft\\DirectPlay\\Service Providers";
    LPCSTR guidDataSubKey  = "Guid";
    char subKeyName[51];
    DWORD dwIndex, sizeOfSubKeyName=50;
    FILETIME filetime;

    /* Need to loop over the service providers in the registry */
    if( RegOpenKeyExA( HKEY_LOCAL_MACHINE, searchSubKey,
                         0, KEY_READ, &hkResult ) != ERROR_SUCCESS )
    {
      /* Hmmm. Does this mean that there are no service providers? */
      ERR(": no service providers?\n");
      return DP_OK;
    }


    /* Traverse all the service providers we have available */
    for( dwIndex=0;
         RegEnumKeyExA( hkResult, dwIndex, subKeyName, &sizeOfSubKeyName,
                        NULL, NULL, NULL, &filetime ) != ERROR_NO_MORE_ITEMS;
         ++dwIndex, sizeOfSubKeyName=51 )
    {

      HKEY     hkServiceProvider;
      GUID     serviceProviderGUID;
      DWORD    returnTypeGUID, sizeOfReturnBuffer = 50;
      char     returnBuffer[51];
      WCHAR    buff[51];
      DPNAME   dpName;
      BOOL     bBuildPass;

      LPVOID                   lpAddressBuffer = NULL;
      DWORD                    dwAddressBufferSize = 0;

      TRACE(" this time through: %s\n", subKeyName );

      /* Get a handle for this particular service provider */
      if( RegOpenKeyExA( hkResult, subKeyName, 0, KEY_READ,
                         &hkServiceProvider ) != ERROR_SUCCESS )
      {
         ERR(": what the heck is going on?\n" );
         continue;
      }

      if( RegQueryValueExA( hkServiceProvider, guidDataSubKey,
                            NULL, &returnTypeGUID, (LPBYTE)returnBuffer,
                            &sizeOfReturnBuffer ) != ERROR_SUCCESS )
      {
        ERR(": missing GUID registry data members\n" );
        RegCloseKey(hkServiceProvider);
        continue;
      }
      RegCloseKey(hkServiceProvider);

      /* FIXME: Check return types to ensure we're interpreting data right */
      MultiByteToWideChar( CP_ACP, 0, returnBuffer, -1, buff, ARRAY_SIZE( buff ));
      CLSIDFromString( buff, &serviceProviderGUID );
      /* FIXME: Have I got a memory leak on the serviceProviderGUID? */

      /* Fill in the DPNAME struct for the service provider */
      dpName.dwSize             = sizeof( dpName );
      dpName.dwFlags            = 0;
      dpName.lpszShortNameA = subKeyName;
      dpName.lpszLongNameA  = NULL;

      /* Create the compound address for the service provider.
       * NOTE: This is a gruesome architectural scar right now.  DP
       * uses DPL and DPL uses DP.  Nasty stuff. This may be why the
       * native dll just gets around this little bit by allocating an
       * 80 byte buffer which isn't even filled with a valid compound
       * address. Oh well. Creating a proper compound address is the
       * way to go anyways despite this method taking slightly more
       * heap space and realtime :) */

      bBuildPass = DP_BuildSPCompoundAddr( &serviceProviderGUID,
                                           &lpAddressBuffer,
                                           &dwAddressBufferSize );
      if( !bBuildPass )
      {
        ERR( "Can't build compound addr\n" );
        return DPERR_GENERIC;
      }

      /* The enumeration will return FALSE if we are not to continue */
      if( !lpEnumCallback( &serviceProviderGUID, lpAddressBuffer, dwAddressBufferSize,
                           &dpName, dwFlags, lpContext ) )
      {
         free( lpAddressBuffer );
         return DP_OK;
      }
      free( lpAddressBuffer );
    }
  }

  /* Enumerate DirectPlayLobby service providers */
  if( dwFlags & DPCONNECTION_DIRECTPLAYLOBBY )
  {
    HKEY hkResult;
    LPCSTR searchSubKey    = "SOFTWARE\\Microsoft\\DirectPlay\\Lobby Providers";
    LPCSTR guidDataSubKey  = "Guid";
    char subKeyName[51];
    DWORD dwIndex, sizeOfSubKeyName=50;
    FILETIME filetime;

    /* Need to loop over the service providers in the registry */
    if( RegOpenKeyExA( HKEY_LOCAL_MACHINE, searchSubKey,
                         0, KEY_READ, &hkResult ) != ERROR_SUCCESS )
    {
      TRACE("No Lobby Providers have been registered.\n");
      return DP_OK;
    }


    /* Traverse all the lobby providers we have available */
    for( dwIndex=0;
         RegEnumKeyExA( hkResult, dwIndex, subKeyName, &sizeOfSubKeyName,
                        NULL, NULL, NULL, &filetime ) != ERROR_NO_MORE_ITEMS;
         ++dwIndex, sizeOfSubKeyName=51 )
    {

      HKEY     hkServiceProvider;
      GUID     serviceProviderGUID;
      DWORD    returnTypeGUID, sizeOfReturnBuffer = 50;
      char     returnBuffer[51];
      WCHAR    buff[51];
      DPNAME   dpName;
      HRESULT  hr;

      DPCOMPOUNDADDRESSELEMENT dpCompoundAddress;
      LPVOID                   lpAddressBuffer = NULL;
      DWORD                    dwAddressBufferSize = 0;

      TRACE(" this time through: %s\n", subKeyName );

      /* Get a handle for this particular service provider */
      if( RegOpenKeyExA( hkResult, subKeyName, 0, KEY_READ,
                         &hkServiceProvider ) != ERROR_SUCCESS )
      {
         ERR(": what the heck is going on?\n" );
         continue;
      }

      if( RegQueryValueExA( hkServiceProvider, guidDataSubKey,
                            NULL, &returnTypeGUID, (LPBYTE)returnBuffer,
                            &sizeOfReturnBuffer ) != ERROR_SUCCESS )
      {
        ERR(": missing GUID registry data members\n" );
        RegCloseKey(hkServiceProvider);
        continue;
      }
      RegCloseKey(hkServiceProvider);

      /* FIXME: Check return types to ensure we're interpreting data right */
      MultiByteToWideChar( CP_ACP, 0, returnBuffer, -1, buff, ARRAY_SIZE( buff ));
      CLSIDFromString( buff, &serviceProviderGUID );
      /* FIXME: Have I got a memory leak on the serviceProviderGUID? */

      /* Fill in the DPNAME struct for the service provider */
      dpName.dwSize             = sizeof( dpName );
      dpName.dwFlags            = 0;
      dpName.lpszShortNameA = subKeyName;
      dpName.lpszLongNameA  = NULL;

      /* Create the compound address for the service provider.
         NOTE: This is a gruesome architectural scar right now. DP uses DPL and DPL uses DP
               nast stuff. This may be why the native dll just gets around this little bit by
               allocating an 80 byte buffer which isn't even a filled with a valid compound
               address. Oh well. Creating a proper compound address is the way to go anyways
               despite this method taking slightly more heap space and realtime :) */

      dpCompoundAddress.guidDataType = DPAID_LobbyProvider;
      dpCompoundAddress.dwDataSize   = sizeof( GUID );
      dpCompoundAddress.lpData       = &serviceProviderGUID;

      if( ( hr = DPL_CreateCompoundAddress( &dpCompoundAddress, 1, lpAddressBuffer,
                                     &dwAddressBufferSize, TRUE ) ) != DPERR_BUFFERTOOSMALL )
      {
        ERR( "can't get buffer size: %s\n", DPLAYX_HresultToString( hr ) );
        return hr;
      }

      /* Now allocate the buffer */
      lpAddressBuffer = calloc( 1, dwAddressBufferSize );

      if( ( hr = DPL_CreateCompoundAddress( &dpCompoundAddress, 1, lpAddressBuffer,
                                     &dwAddressBufferSize, TRUE ) ) != DP_OK )
      {
        ERR( "can't create address: %s\n", DPLAYX_HresultToString( hr ) );
        free( lpAddressBuffer );
        return hr;
      }

      /* The enumeration will return FALSE if we are not to continue */
      if( !lpEnumCallback( &serviceProviderGUID, lpAddressBuffer, dwAddressBufferSize,
                           &dpName, dwFlags, lpContext ) )
      {
         free( lpAddressBuffer );
         return DP_OK;
      }
      free( lpAddressBuffer );
    }
  }

  return DP_OK;
}

static HRESULT WINAPI IDirectPlay4Impl_EnumConnections( IDirectPlay4 *iface,
        const GUID *application, LPDPENUMCONNECTIONSCALLBACK enumcb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    return IDirectPlayX_EnumConnections( &This->IDirectPlay4A_iface, application, enumcb, context,
            flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_EnumGroupsInGroup( IDirectPlay3A *iface, DPID group,
        GUID *instance, LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_EnumGroupsInGroup( &This->IDirectPlay4A_iface, group, instance,
            enumplayercb, context, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_EnumGroupsInGroup( IDirectPlay3 *iface, DPID group,
        GUID *instance, LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_EnumGroupsInGroup( &This->IDirectPlay4_iface, group, instance,
            enumplayercb, context, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_EnumGroupsInGroup( IDirectPlay4A *iface, DPID group,
        GUID *instance, LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_EnumGroupsInGroup( &This->IDirectPlay4_iface, group, instance,
            enumplayercb, context, flags );
}

static HRESULT WINAPI IDirectPlay4Impl_EnumGroupsInGroup( IDirectPlay4 *iface, DPID group,
        GUID *instance, LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    lpGroupList glist;
    lpGroupData gdata;

    FIXME( "(%p)->(0x%08lx,%p,%p,%p,0x%08lx): semi stub\n", This, group, instance, enumplayercb,
            context, flags );

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;

    if ( ( gdata = DP_FindAnyGroup(This, group ) ) == NULL )
        return DPERR_INVALIDGROUP;

    if ( DPQ_IS_EMPTY( gdata->groups ) )
        return DP_OK;


    for( glist = DPQ_FIRST( gdata->groups ); ; glist = DPQ_NEXT( glist->groups ) )
    {
        /* FIXME: Should check flags for match here */
        if ( !(*enumplayercb)( glist->lpGData->dpid, DPPLAYERTYPE_GROUP, &glist->lpGData->name,
                    flags, context ) )
            return DP_OK; /* User requested break */

        if ( DPQ_IS_ENDOFLIST( glist->groups ) )
            break;
    }

    return DP_OK;
}

static HRESULT WINAPI IDirectPlay3AImpl_GetGroupConnectionSettings( IDirectPlay3A *iface,
        DWORD flags, DPID group, void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_GetGroupConnectionSettings( &This->IDirectPlay4A_iface, flags, group,
            data, size );
}

static HRESULT WINAPI IDirectPlay3Impl_GetGroupConnectionSettings( IDirectPlay3 *iface,
        DWORD flags, DPID group, void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_GetGroupConnectionSettings( &This->IDirectPlay4_iface, flags, group,
            data, size );
}

static HRESULT WINAPI IDirectPlay4AImpl_GetGroupConnectionSettings( IDirectPlay4A *iface,
        DWORD flags, DPID group, void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    FIXME("(%p)->(0x%08lx,0x%08lx,%p,%p): stub\n", This, flags, group, data, size );
    return DP_OK;
}

static HRESULT WINAPI IDirectPlay4Impl_GetGroupConnectionSettings( IDirectPlay4 *iface, DWORD flags,
        DPID group, void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    FIXME( "(%p)->(0x%08lx,0x%08lx,%p,%p): stub\n", This, flags, group, data, size );
    return DP_OK;
}

static BOOL CALLBACK DP_GetSpLpGuidFromCompoundAddress(
    REFGUID         guidDataType,
    DWORD           dwDataSize,
    LPCVOID         lpData,
    LPVOID          lpContext )
{
  /* Looking for the GUID of the provider to load */
  if( ( IsEqualGUID( guidDataType, &DPAID_ServiceProvider ) ) ||
      ( IsEqualGUID( guidDataType, &DPAID_LobbyProvider ) )
    )
  {
    TRACE( "Found SP/LP (%s) %s (data size = 0x%08lx)\n",
           debugstr_guid( guidDataType ), debugstr_guid( lpData ), dwDataSize );

    if( dwDataSize != sizeof( GUID ) )
    {
      ERR( "Invalid sp/lp guid size 0x%08lx\n", dwDataSize );
    }

    memcpy( lpContext, lpData, dwDataSize );

    /* There shouldn't be more than 1 GUID/compound address */
    return FALSE;
  }

  /* Still waiting for what we want */
  return TRUE;
}


/* Find and perform a LoadLibrary on the requested SP or LP GUID */
static HMODULE DP_LoadSP( LPCGUID lpcGuid, LPSPINITDATA lpSpData, LPBOOL lpbIsDpSp )
{
  UINT i;
  LPCSTR spSubKey         = "SOFTWARE\\Microsoft\\DirectPlay\\Service Providers";
  LPCSTR lpSubKey         = "SOFTWARE\\Microsoft\\DirectPlay\\Lobby Providers";
  LPCSTR guidDataSubKey   = "Guid";
  LPCSTR majVerDataSubKey = "dwReserved1";
  LPCSTR minVerDataSubKey = "dwReserved2";
  LPCSTR pathSubKey       = "Path";

  TRACE( " request to load %s\n", debugstr_guid( lpcGuid ) );

  /* FIXME: Cloned code with a quick hack. */
  for( i=0; i<2; i++ )
  {
    HKEY hkResult;
    LPCSTR searchSubKey;
    char subKeyName[51];
    DWORD dwIndex, sizeOfSubKeyName=50;
    FILETIME filetime;

    (i == 0) ? (searchSubKey = spSubKey ) : (searchSubKey = lpSubKey );
    *lpbIsDpSp = (i == 0);


    /* Need to loop over the service providers in the registry */
    if( RegOpenKeyExA( HKEY_LOCAL_MACHINE, searchSubKey,
                         0, KEY_READ, &hkResult ) != ERROR_SUCCESS )
    {
      /* Hmmm. Does this mean that there are no service providers? */
      ERR(": no service providers?\n");
      return 0;
    }

    /* Traverse all the service providers we have available */
    for( dwIndex=0;
         RegEnumKeyExA( hkResult, dwIndex, subKeyName, &sizeOfSubKeyName,
                        NULL, NULL, NULL, &filetime ) != ERROR_NO_MORE_ITEMS;
         ++dwIndex, sizeOfSubKeyName=51 )
    {

      HKEY     hkServiceProvider;
      GUID     serviceProviderGUID;
      DWORD    returnType, sizeOfReturnBuffer = 255;
      char     returnBuffer[256];
      WCHAR    buff[51];
      DWORD    dwTemp, len;

      TRACE(" this time through: %s\n", subKeyName );

      /* Get a handle for this particular service provider */
      if( RegOpenKeyExA( hkResult, subKeyName, 0, KEY_READ,
                         &hkServiceProvider ) != ERROR_SUCCESS )
      {
         ERR(": what the heck is going on?\n" );
         continue;
      }

      if( RegQueryValueExA( hkServiceProvider, guidDataSubKey,
                            NULL, &returnType, (LPBYTE)returnBuffer,
                            &sizeOfReturnBuffer ) != ERROR_SUCCESS )
      {
        ERR(": missing GUID registry data members\n" );
        continue;
      }

      /* FIXME: Check return types to ensure we're interpreting data right */
      MultiByteToWideChar( CP_ACP, 0, returnBuffer, -1, buff, ARRAY_SIZE( buff ));
      CLSIDFromString( buff, &serviceProviderGUID );
      /* FIXME: Have I got a memory leak on the serviceProviderGUID? */

      /* Determine if this is the Service Provider that the user asked for */
      if( !IsEqualGUID( &serviceProviderGUID, lpcGuid ) )
      {
        continue;
      }

      if( i == 0 ) /* DP SP */
      {
        len = MultiByteToWideChar( CP_ACP, 0, subKeyName, -1, NULL, 0 );
        lpSpData->lpszName = malloc( len * sizeof(WCHAR) );
        MultiByteToWideChar( CP_ACP, 0, subKeyName, -1, lpSpData->lpszName, len );
      }

      sizeOfReturnBuffer = 255;

      /* Get dwReserved1 */
      if( RegQueryValueExA( hkServiceProvider, majVerDataSubKey,
                            NULL, &returnType, (LPBYTE)returnBuffer,
                            &sizeOfReturnBuffer ) != ERROR_SUCCESS )
      {
         ERR(": missing dwReserved1 registry data members\n") ;
         continue;
      }

      if( i == 0 )
          memcpy( &lpSpData->dwReserved1, returnBuffer, sizeof(lpSpData->dwReserved1) );

      sizeOfReturnBuffer = 255;

      /* Get dwReserved2 */
      if( RegQueryValueExA( hkServiceProvider, minVerDataSubKey,
                            NULL, &returnType, (LPBYTE)returnBuffer,
                            &sizeOfReturnBuffer ) != ERROR_SUCCESS )
      {
         ERR(": missing dwReserved1 registry data members\n") ;
         continue;
      }

      if( i == 0 )
          memcpy( &lpSpData->dwReserved2, returnBuffer, sizeof(lpSpData->dwReserved2) );

      sizeOfReturnBuffer = 255;

      /* Get the path for this service provider */
      if( ( dwTemp = RegQueryValueExA( hkServiceProvider, pathSubKey,
                            NULL, NULL, (LPBYTE)returnBuffer,
                            &sizeOfReturnBuffer ) ) != ERROR_SUCCESS )
      {
        ERR(": missing PATH registry data members: 0x%08lx\n", dwTemp );
        continue;
      }

      TRACE( "Loading %s\n", returnBuffer );
      return LoadLibraryA( returnBuffer );
    }
  }

  return 0;
}

static HRESULT DP_InitializeDPSP( IDirectPlayImpl *This, HMODULE hServiceProvider )
{
  HRESULT hr;
  LPDPSP_SPINIT SPInit;

  /* Initialize the service provider by calling SPInit */
  SPInit = (LPDPSP_SPINIT)GetProcAddress( hServiceProvider, "SPInit" );

  if( SPInit == NULL )
  {
    ERR( "Service provider doesn't provide SPInit interface?\n" );
    FreeLibrary( hServiceProvider );
    return DPERR_UNAVAILABLE;
  }

  TRACE( "Calling SPInit (DP SP entry point)\n" );

  hr = (*SPInit)( &This->dp2->spData );

  if( FAILED(hr) )
  {
    ERR( "DP SP Initialization failed: %s\n", DPLAYX_HresultToString(hr) );
    FreeLibrary( hServiceProvider );
    return hr;
  }

  /* FIXME: Need to verify the sanity of the returned callback table
   *        using IsBadCodePtr */
  This->dp2->bSPInitialized = TRUE;

  /* This interface is now initialized as a DP object */
  This->dp2->connectionInitialized = DP_SERVICE_PROVIDER;

  /* Store the handle of the module so that we can unload it later */
  This->dp2->hServiceProvider = hServiceProvider;

  return hr;
}

static HRESULT DP_InitializeDPLSP( IDirectPlayImpl *This, HMODULE hLobbyProvider )
{
  HRESULT hr;
  LPSP_INIT DPLSPInit;

  /* Initialize the service provider by calling SPInit */
  DPLSPInit = (LPSP_INIT)GetProcAddress( hLobbyProvider, "DPLSPInit" );

  if( DPLSPInit == NULL )
  {
    ERR( "Service provider doesn't provide DPLSPInit interface?\n" );
    FreeLibrary( hLobbyProvider );
    return DPERR_UNAVAILABLE;
  }

  TRACE( "Calling DPLSPInit (DPL SP entry point)\n" );

  hr = (*DPLSPInit)( &This->dp2->dplspData );

  if( FAILED(hr) )
  {
    ERR( "DPL SP Initialization failed: %s\n", DPLAYX_HresultToString(hr) );
    FreeLibrary( hLobbyProvider );
    return hr;
  }

  /* FIXME: Need to verify the sanity of the returned callback table
   *        using IsBadCodePtr */

  This->dp2->bDPLSPInitialized = TRUE;

  /* This interface is now initialized as a lobby object */
  This->dp2->connectionInitialized = DP_LOBBY_PROVIDER;

  /* Store the handle of the module so that we can unload it later */
  This->dp2->hDPLobbyProvider = hLobbyProvider;

  return hr;
}

static HRESULT WINAPI IDirectPlay3AImpl_InitializeConnection( IDirectPlay3A *iface,
        void *connection, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_InitializeConnection( &This->IDirectPlay4A_iface, connection, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_InitializeConnection( IDirectPlay3 *iface,
        void *connection, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_InitializeConnection( &This->IDirectPlay4_iface, connection, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_InitializeConnection( IDirectPlay4A *iface,
        void *connection, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_InitializeConnection( &This->IDirectPlay4_iface, connection, flags );
}

static HRESULT WINAPI IDirectPlay4Impl_InitializeConnection( IDirectPlay4 *iface,
        void *connection, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    HMODULE servprov;
    GUID sp;
    const DWORD size = 80; /* FIXME: Need to calculate it correctly */
    BOOL is_dp_sp; /* TRUE if Direct Play SP, FALSE if Direct Play Lobby SP */
    HRESULT hr;

    TRACE( "(%p)->(%p,0x%08lx)\n", This, connection, flags );

    if ( !connection )
        return DPERR_INVALIDPARAMS;

    if ( flags )
        return DPERR_INVALIDFLAGS;

    if ( This->dp2->connectionInitialized != NO_PROVIDER )
        return DPERR_ALREADYINITIALIZED;

    /* Find out what the requested SP is and how large this buffer is */
    hr = DPL_EnumAddress( DP_GetSpLpGuidFromCompoundAddress, connection, size, &sp );

    if ( FAILED(hr) )
    {
        ERR( "Invalid compound address?\n" );
        return DPERR_UNAVAILABLE;
    }

    /* Load the service provider */
    servprov = DP_LoadSP( &sp, &This->dp2->spData, &is_dp_sp );

    if ( !servprov )
    {
        ERR( "Unable to load service provider %s\n", debugstr_guid(&sp) );
        return DPERR_UNAVAILABLE;
    }

    if ( is_dp_sp )
    {
         /* Fill in what we can of the Service Provider required information.
          * The rest was be done in DP_LoadSP
          */
         This->dp2->spData.lpAddress = connection;
         This->dp2->spData.dwAddressSize = size;
         This->dp2->spData.lpGuid = &sp;
         hr = DP_InitializeDPSP( This, servprov );
    }
    else
    {
         This->dp2->dplspData.lpAddress = connection;
         hr = DP_InitializeDPLSP( This, servprov );
    }

    if ( FAILED(hr) )
        return hr;

    return DP_OK;
}

static HRESULT WINAPI IDirectPlay3AImpl_SecureOpen( IDirectPlay3A *iface,
        const DPSESSIONDESC2 *sdesc, DWORD flags, const DPSECURITYDESC *security,
        const DPCREDENTIALS *credentials )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_SecureOpen( &This->IDirectPlay4A_iface, sdesc, flags, security,
            credentials );
}

static HRESULT WINAPI IDirectPlay3Impl_SecureOpen( IDirectPlay3 *iface,
        const DPSESSIONDESC2 *sdesc, DWORD flags, const DPSECURITYDESC *security,
        const DPCREDENTIALS *credentials )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_SecureOpen( &This->IDirectPlay4_iface, sdesc, flags, security,
            credentials );
}

static HRESULT WINAPI IDirectPlay4AImpl_SecureOpen( IDirectPlay4A *iface,
        const DPSESSIONDESC2 *lpsd, DWORD dwFlags, const DPSECURITYDESC *lpSecurity,
        const DPCREDENTIALS *lpCredentials )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return DP_SecureOpen( This, lpsd, dwFlags, lpSecurity, lpCredentials, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_SecureOpen( IDirectPlay4 *iface,
        const DPSESSIONDESC2 *lpsd, DWORD dwFlags, const DPSECURITYDESC *lpSecurity,
        const DPCREDENTIALS *lpCredentials )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    return DP_SecureOpen( This, lpsd, dwFlags, lpSecurity, lpCredentials, FALSE );
}

static HRESULT WINAPI IDirectPlay3AImpl_SendChatMessage( IDirectPlay3A *iface, DPID from, DPID to,
        DWORD flags, DPCHAT *chatmsg )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_SendChatMessage( &This->IDirectPlay4A_iface, from, to, flags, chatmsg );
}

static HRESULT WINAPI IDirectPlay3Impl_SendChatMessage( IDirectPlay3 *iface, DPID from, DPID to,
        DWORD flags, DPCHAT *chatmsg )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_SendChatMessage( &This->IDirectPlay4_iface, from, to, flags, chatmsg );
}

static HRESULT WINAPI IDirectPlay4AImpl_SendChatMessage( IDirectPlay4A *iface, DPID from,
        DPID to, DWORD flags, DPCHAT *chatmsg )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    FIXME("(%p)->(0x%08lx,0x%08lx,0x%08lx,%p): stub\n", This, from, to, flags, chatmsg );
    return DP_OK;
}

static HRESULT WINAPI IDirectPlay4Impl_SendChatMessage( IDirectPlay4 *iface, DPID from, DPID to,
        DWORD flags, DPCHAT *chatmsg )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    FIXME( "(%p)->(0x%08lx,0x%08lx,0x%08lx,%p): stub\n", This, from, to, flags, chatmsg );
    return DP_OK;
}

static HRESULT WINAPI IDirectPlay3AImpl_SetGroupConnectionSettings( IDirectPlay3A *iface,
        DWORD flags, DPID group, DPLCONNECTION *connection )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_SetGroupConnectionSettings( &This->IDirectPlay4A_iface, flags, group,
            connection );
}

static HRESULT WINAPI IDirectPlay3Impl_SetGroupConnectionSettings( IDirectPlay3 *iface,
        DWORD flags, DPID group, DPLCONNECTION *connection )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_SetGroupConnectionSettings( &This->IDirectPlay4_iface, flags, group,
            connection );
}

static HRESULT WINAPI IDirectPlay4AImpl_SetGroupConnectionSettings( IDirectPlay4A *iface,
        DWORD flags, DPID group, DPLCONNECTION *connection )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    FIXME("(%p)->(0x%08lx,0x%08lx,%p): stub\n", This, flags, group, connection );
    return DP_OK;
}

static HRESULT WINAPI IDirectPlay4Impl_SetGroupConnectionSettings( IDirectPlay4 *iface, DWORD flags,
        DPID group, DPLCONNECTION *connection )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    FIXME( "(%p)->(0x%08lx,0x%08lx,%p): stub\n", This, flags, group, connection );
    return DP_OK;
}

static HRESULT WINAPI IDirectPlay3AImpl_StartSession( IDirectPlay3A *iface, DWORD flags,
        DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_StartSession( &This->IDirectPlay4A_iface, flags, group );
}

static HRESULT WINAPI IDirectPlay3Impl_StartSession( IDirectPlay3 *iface, DWORD flags,
        DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_StartSession( &This->IDirectPlay4_iface, flags, group );
}

static HRESULT WINAPI IDirectPlay4AImpl_StartSession( IDirectPlay4A *iface, DWORD flags,
        DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_StartSession( &This->IDirectPlay4_iface, flags, group );
}

static HRESULT WINAPI IDirectPlay4Impl_StartSession( IDirectPlay4 *iface, DWORD flags, DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    FIXME( "(%p)->(0x%08lx,0x%08lx): stub\n", This, flags, group );
    return DP_OK;
}

static HRESULT WINAPI IDirectPlay3AImpl_GetGroupFlags( IDirectPlay3A *iface, DPID group,
        DWORD *flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_GetGroupFlags( &This->IDirectPlay4A_iface, group, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_GetGroupFlags( IDirectPlay3 *iface, DPID group,
        DWORD *flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_GetGroupFlags( &This->IDirectPlay4_iface, group, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_GetGroupFlags( IDirectPlay4A *iface, DPID group,
        DWORD *flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_GetGroupFlags( &This->IDirectPlay4_iface, group, flags );
}

static HRESULT WINAPI IDirectPlay4Impl_GetGroupFlags( IDirectPlay4 *iface, DPID group,
        DWORD *flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    FIXME( "(%p)->(0x%08lx,%p): stub\n", This, group, flags );
    return DP_OK;
}

static HRESULT WINAPI IDirectPlay3AImpl_GetGroupParent( IDirectPlay3A *iface, DPID group,
        DPID *parent )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_GetGroupParent( &This->IDirectPlay4A_iface, group, parent );
}

static HRESULT WINAPI IDirectPlay3Impl_GetGroupParent( IDirectPlay3 *iface, DPID group,
        DPID *parent )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_GetGroupParent( &This->IDirectPlay4_iface, group, parent );
}

static HRESULT WINAPI IDirectPlay4AImpl_GetGroupParent( IDirectPlay4A *iface, DPID group,
        DPID *parent )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_GetGroupParent( &This->IDirectPlay4_iface, group, parent );
}

static HRESULT WINAPI IDirectPlay4Impl_GetGroupParent( IDirectPlay4 *iface, DPID group,
        DPID *parent )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    lpGroupData gdata;

    TRACE( "(%p)->(0x%08lx,%p)\n", This, group, parent );

    if ( ( gdata = DP_FindAnyGroup( This, group ) ) == NULL )
        return DPERR_INVALIDGROUP;

    *parent = gdata->dpid;

    return DP_OK;
}

static HRESULT WINAPI IDirectPlay3AImpl_GetPlayerAccount( IDirectPlay3A *iface, DPID player,
        DWORD flags, void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_GetPlayerAccount( &This->IDirectPlay4A_iface, player, flags, data, size );
}

static HRESULT WINAPI IDirectPlay3Impl_GetPlayerAccount( IDirectPlay3 *iface, DPID player,
        DWORD flags, void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_GetPlayerAccount( &This->IDirectPlay4_iface, player, flags, data, size );
}

static HRESULT WINAPI IDirectPlay4AImpl_GetPlayerAccount( IDirectPlay4A *iface, DPID player,
        DWORD flags, void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    FIXME("(%p)->(0x%08lx,0x%08lx,%p,%p): stub\n", This, player, flags, data, size );
    return DP_OK;
}

static HRESULT WINAPI IDirectPlay4Impl_GetPlayerAccount( IDirectPlay4 *iface, DPID player,
        DWORD flags, void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    FIXME( "(%p)->(0x%08lx,0x%08lx,%p,%p): stub\n", This, player, flags, data, size );
    return DP_OK;
}

static HRESULT WINAPI IDirectPlay3AImpl_GetPlayerFlags( IDirectPlay3A *iface, DPID player,
        DWORD *flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_GetPlayerFlags( &This->IDirectPlay4A_iface, player, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_GetPlayerFlags( IDirectPlay3 *iface, DPID player,
        DWORD *flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_GetPlayerFlags( &This->IDirectPlay4_iface, player, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_GetPlayerFlags( IDirectPlay4A *iface, DPID player,
        DWORD *flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_GetPlayerFlags( &This->IDirectPlay4_iface, player, flags );
}

static HRESULT WINAPI IDirectPlay4Impl_GetPlayerFlags( IDirectPlay4 *iface, DPID player,
        DWORD *flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    FIXME( "(%p)->(0x%08lx,%p): stub\n", This, player, flags );
    return DP_OK;
}

static HRESULT WINAPI IDirectPlay4AImpl_GetGroupOwner( IDirectPlay4A *iface, DPID group,
        DPID *owner )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_GetGroupOwner( &This->IDirectPlay4_iface, group, owner );
}

static HRESULT WINAPI IDirectPlay4Impl_GetGroupOwner( IDirectPlay4 *iface, DPID group,
        DPID *owner )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    FIXME( "(%p)->(0x%08lx,%p): stub\n", This, group, owner );
    return DP_OK;
}

static HRESULT WINAPI IDirectPlay4AImpl_SetGroupOwner( IDirectPlay4A *iface, DPID group,
        DPID owner )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_SetGroupOwner( &This->IDirectPlay4_iface, group, owner );
}

static HRESULT WINAPI IDirectPlay4Impl_SetGroupOwner( IDirectPlay4 *iface, DPID group ,
        DPID owner )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    FIXME( "(%p)->(0x%08lx,0x%08lx): stub\n", This, group, owner );
    return DP_OK;
}

static HRESULT WINAPI IDirectPlay4AImpl_SendEx( IDirectPlay4A *iface, DPID from, DPID to,
        DWORD flags, void *data, DWORD size, DWORD priority, DWORD timeout, void *context,
        DWORD *msgid )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_SendEx( &This->IDirectPlay4_iface, from, to, flags, data, size, priority,
            timeout, context, msgid );
}

static HRESULT WINAPI IDirectPlay4Impl_SendEx( IDirectPlay4 *iface, DPID from, DPID to,
        DWORD flags, void *data, DWORD size, DWORD priority, DWORD timeout, void *context,
        DWORD *msgid )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );

    FIXME( "(%p)->(0x%08lx,0x%08lx,0x%08lx,%p,0x%08lx,0x%08lx,0x%08lx,%p,%p): semi-stub\n",
            This, from, to, flags, data, size, priority, timeout, context, msgid );

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;

    /* FIXME: Add parameter checking */
    /* FIXME: First call to this needs to acquire a message id which will be
     *        used for multiple sends
     */

    /* NOTE: Can't send messages to yourself - this will be trapped in receive */

    /* Verify that the message is being sent from a valid local player. The
     * from player may be anonymous DPID_UNKNOWN
     */
    if ( from != DPID_UNKNOWN && !DP_FindPlayer( This, from ) )
    {
        WARN( "INFO: Invalid from player 0x%08lx\n", from );
        return DPERR_INVALIDPLAYER;
    }

    /* Verify that the message is being sent to a valid player, group or to
     * everyone. If it's valid, send it to those players.
     */
    if ( to == DPID_ALLPLAYERS )
    {
        /* See if SP has the ability to multicast. If so, use it */
        if ( This->dp2->spData.lpCB->SendToGroupEx )
            FIXME( "Use group sendex to group 0\n" );
        else if ( This->dp2->spData.lpCB->SendToGroup ) /* obsolete interface */
            FIXME( "Use obsolete group send to group 0\n" );
        else /* No multicast, multiplicate */
            FIXME( "Send to all players using EnumPlayersInGroup\n" );
    }
    else if ( DP_FindPlayer( This, to ) )
    {
        /* Have the service provider send this message */
        /* FIXME: Could optimize for local interface sends */
        return DP_SP_SendEx( This, flags, data, size, priority, timeout, context, msgid );
    }
    else if ( DP_FindAnyGroup( This, to ) )
    {
        /* See if SP has the ability to multicast. If so, use it */
        if ( This->dp2->spData.lpCB->SendToGroupEx )
            FIXME( "Use group sendex\n" );
        else if ( This->dp2->spData.lpCB->SendToGroup ) /* obsolete interface */
            FIXME( "Use obsolete group send to group\n" );
        else /* No multicast, multiplicate */
            FIXME( "Send to all players using EnumPlayersInGroup\n" );

    }
    else
        return DPERR_INVALIDPLAYER;

    /* FIXME: Should return what the send returned */
    return DP_OK;
}

static HRESULT DP_SP_SendEx( IDirectPlayImpl *This, DWORD dwFlags, void *lpData, DWORD dwDataSize,
        DWORD dwPriority, DWORD dwTimeout, void *lpContext, DWORD *lpdwMsgID )
{
  LPDPMSG lpMElem;

  FIXME( ": stub\n" );

  /* FIXME: This queuing should only be for async messages */

  lpMElem = calloc( 1, sizeof( *lpMElem ) );
  lpMElem->msg = malloc( dwDataSize );

  CopyMemory( lpMElem->msg, lpData, dwDataSize );

  /* FIXME: Need to queue based on priority */
  DPQ_INSERT( This->dp2->sendMsgs, lpMElem, msgs );

  return DP_OK;
}

static HRESULT WINAPI IDirectPlay4AImpl_GetMessageQueue( IDirectPlay4A *iface, DPID from, DPID to,
        DWORD flags, DWORD *msgs, DWORD *bytes )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_GetMessageQueue( &This->IDirectPlay4_iface, from, to, flags, msgs, bytes );
}

static HRESULT WINAPI IDirectPlay4Impl_GetMessageQueue( IDirectPlay4 *iface, DPID from, DPID to,
        DWORD flags, DWORD *msgs, DWORD *bytes )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    HRESULT hr = DP_OK;

    FIXME( "(%p)->(0x%08lx,0x%08lx,0x%08lx,%p,%p): semi-stub\n", This, from, to, flags, msgs, bytes );

    /* FIXME: Do we need to do from and to sanity checking here? */
    /* FIXME: What about sends which are not immediate? */

    if ( This->dp2->spData.lpCB->GetMessageQueue )
    {
        DPSP_GETMESSAGEQUEUEDATA data;

        FIXME( "Calling SP GetMessageQueue - is it right?\n" );

        /* FIXME: None of this is documented :( */
        data.lpISP        = This->dp2->spData.lpISP;
        data.dwFlags      = flags;
        data.idFrom       = from;
        data.idTo         = to;
        data.lpdwNumMsgs  = msgs;
        data.lpdwNumBytes = bytes;

        hr = (*This->dp2->spData.lpCB->GetMessageQueue)( &data );
    }
    else
        FIXME( "No SP for GetMessageQueue - fake some data\n" );

    return hr;
}

static HRESULT dplay_cancelmsg ( IDirectPlayImpl* This, DWORD msgid, DWORD flags, DWORD minprio,
        DWORD maxprio )
{
    HRESULT hr = DP_OK;

    FIXME( "(%p)->(0x%08lx,0x%08lx): semi stub\n", This, msgid, flags );

    if ( This->dp2->spData.lpCB->Cancel )
    {
        DPSP_CANCELDATA data;

        TRACE( "Calling SP Cancel\n" );

        /* FIXME: Undocumented callback */

        data.lpISP          = This->dp2->spData.lpISP;
        data.dwFlags        = flags;
        data.lprglpvSPMsgID = NULL;
        data.cSPMsgID       = msgid;
        data.dwMinPriority  = minprio;
        data.dwMaxPriority  = maxprio;

        hr = (*This->dp2->spData.lpCB->Cancel)( &data );
    }
    else
        FIXME( "SP doesn't implement Cancel\n" );

    return hr;
}

static HRESULT WINAPI IDirectPlay4AImpl_CancelMessage( IDirectPlay4A *iface, DWORD msgid,
        DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_CancelMessage( &This->IDirectPlay4_iface, msgid, flags );
}

static HRESULT WINAPI IDirectPlay4Impl_CancelMessage( IDirectPlay4 *iface, DWORD msgid,
        DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );

    if ( flags != 0 )
      return DPERR_INVALIDFLAGS;

    if ( msgid == 0 )
      flags |= DPCANCELSEND_ALL;

    return dplay_cancelmsg( This, msgid, flags, 0, 0 );
}

static HRESULT WINAPI IDirectPlay4AImpl_CancelPriority( IDirectPlay4A *iface, DWORD minprio,
        DWORD maxprio, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_CancelPriority( &This->IDirectPlay4_iface, minprio, maxprio, flags );
}

static HRESULT WINAPI IDirectPlay4Impl_CancelPriority( IDirectPlay4 *iface, DWORD minprio,
        DWORD maxprio, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );

    if ( flags != 0 )
        return DPERR_INVALIDFLAGS;

    return dplay_cancelmsg( This, 0, DPCANCELSEND_PRIORITY, minprio, maxprio );
}

static const IDirectPlay2Vtbl dp2_vt =
{
    IDirectPlay2Impl_QueryInterface,
    IDirectPlay2Impl_AddRef,
    IDirectPlay2Impl_Release,
    IDirectPlay2Impl_AddPlayerToGroup,
    IDirectPlay2Impl_Close,
    IDirectPlay2Impl_CreateGroup,
    IDirectPlay2Impl_CreatePlayer,
    IDirectPlay2Impl_DeletePlayerFromGroup,
    IDirectPlay2Impl_DestroyGroup,
    IDirectPlay2Impl_DestroyPlayer,
    IDirectPlay2Impl_EnumGroupPlayers,
    IDirectPlay2Impl_EnumGroups,
    IDirectPlay2Impl_EnumPlayers,
    IDirectPlay2Impl_EnumSessions,
    IDirectPlay2Impl_GetCaps,
    IDirectPlay2Impl_GetGroupData,
    IDirectPlay2Impl_GetGroupName,
    IDirectPlay2Impl_GetMessageCount,
    IDirectPlay2Impl_GetPlayerAddress,
    IDirectPlay2Impl_GetPlayerCaps,
    IDirectPlay2Impl_GetPlayerData,
    IDirectPlay2Impl_GetPlayerName,
    IDirectPlay2Impl_GetSessionDesc,
    IDirectPlay2Impl_Initialize,
    IDirectPlay2Impl_Open,
    IDirectPlay2Impl_Receive,
    IDirectPlay2Impl_Send,
    IDirectPlay2Impl_SetGroupData,
    IDirectPlay2Impl_SetGroupName,
    IDirectPlay2Impl_SetPlayerData,
    IDirectPlay2Impl_SetPlayerName,
    IDirectPlay2Impl_SetSessionDesc
};

static const IDirectPlay2Vtbl dp2A_vt =
{
    IDirectPlay2AImpl_QueryInterface,
    IDirectPlay2AImpl_AddRef,
    IDirectPlay2AImpl_Release,
    IDirectPlay2AImpl_AddPlayerToGroup,
    IDirectPlay2AImpl_Close,
    IDirectPlay2AImpl_CreateGroup,
    IDirectPlay2AImpl_CreatePlayer,
    IDirectPlay2AImpl_DeletePlayerFromGroup,
    IDirectPlay2AImpl_DestroyGroup,
    IDirectPlay2AImpl_DestroyPlayer,
    IDirectPlay2AImpl_EnumGroupPlayers,
    IDirectPlay2AImpl_EnumGroups,
    IDirectPlay2AImpl_EnumPlayers,
    IDirectPlay2AImpl_EnumSessions,
    IDirectPlay2AImpl_GetCaps,
    IDirectPlay2AImpl_GetGroupData,
    IDirectPlay2AImpl_GetGroupName,
    IDirectPlay2AImpl_GetMessageCount,
    IDirectPlay2AImpl_GetPlayerAddress,
    IDirectPlay2AImpl_GetPlayerCaps,
    IDirectPlay2AImpl_GetPlayerData,
    IDirectPlay2AImpl_GetPlayerName,
    IDirectPlay2AImpl_GetSessionDesc,
    IDirectPlay2AImpl_Initialize,
    IDirectPlay2AImpl_Open,
    IDirectPlay2AImpl_Receive,
    IDirectPlay2AImpl_Send,
    IDirectPlay2AImpl_SetGroupData,
    IDirectPlay2AImpl_SetGroupName,
    IDirectPlay2AImpl_SetPlayerData,
    IDirectPlay2AImpl_SetPlayerName,
    IDirectPlay2AImpl_SetSessionDesc
};

static const IDirectPlay3Vtbl dp3_vt =
{
    IDirectPlay3Impl_QueryInterface,
    IDirectPlay3Impl_AddRef,
    IDirectPlay3Impl_Release,
    IDirectPlay3Impl_AddPlayerToGroup,
    IDirectPlay3Impl_Close,
    IDirectPlay3Impl_CreateGroup,
    IDirectPlay3Impl_CreatePlayer,
    IDirectPlay3Impl_DeletePlayerFromGroup,
    IDirectPlay3Impl_DestroyGroup,
    IDirectPlay3Impl_DestroyPlayer,
    IDirectPlay3Impl_EnumGroupPlayers,
    IDirectPlay3Impl_EnumGroups,
    IDirectPlay3Impl_EnumPlayers,
    IDirectPlay3Impl_EnumSessions,
    IDirectPlay3Impl_GetCaps,
    IDirectPlay3Impl_GetGroupData,
    IDirectPlay3Impl_GetGroupName,
    IDirectPlay3Impl_GetMessageCount,
    IDirectPlay3Impl_GetPlayerAddress,
    IDirectPlay3Impl_GetPlayerCaps,
    IDirectPlay3Impl_GetPlayerData,
    IDirectPlay3Impl_GetPlayerName,
    IDirectPlay3Impl_GetSessionDesc,
    IDirectPlay3Impl_Initialize,
    IDirectPlay3Impl_Open,
    IDirectPlay3Impl_Receive,
    IDirectPlay3Impl_Send,
    IDirectPlay3Impl_SetGroupData,
    IDirectPlay3Impl_SetGroupName,
    IDirectPlay3Impl_SetPlayerData,
    IDirectPlay3Impl_SetPlayerName,
    IDirectPlay3Impl_SetSessionDesc,
    IDirectPlay3Impl_AddGroupToGroup,
    IDirectPlay3Impl_CreateGroupInGroup,
    IDirectPlay3Impl_DeleteGroupFromGroup,
    IDirectPlay3Impl_EnumConnections,
    IDirectPlay3Impl_EnumGroupsInGroup,
    IDirectPlay3Impl_GetGroupConnectionSettings,
    IDirectPlay3Impl_InitializeConnection,
    IDirectPlay3Impl_SecureOpen,
    IDirectPlay3Impl_SendChatMessage,
    IDirectPlay3Impl_SetGroupConnectionSettings,
    IDirectPlay3Impl_StartSession,
    IDirectPlay3Impl_GetGroupFlags,
    IDirectPlay3Impl_GetGroupParent,
    IDirectPlay3Impl_GetPlayerAccount,
    IDirectPlay3Impl_GetPlayerFlags
};

static const IDirectPlay3Vtbl dp3A_vt =
{
    IDirectPlay3AImpl_QueryInterface,
    IDirectPlay3AImpl_AddRef,
    IDirectPlay3AImpl_Release,
    IDirectPlay3AImpl_AddPlayerToGroup,
    IDirectPlay3AImpl_Close,
    IDirectPlay3AImpl_CreateGroup,
    IDirectPlay3AImpl_CreatePlayer,
    IDirectPlay3AImpl_DeletePlayerFromGroup,
    IDirectPlay3AImpl_DestroyGroup,
    IDirectPlay3AImpl_DestroyPlayer,
    IDirectPlay3AImpl_EnumGroupPlayers,
    IDirectPlay3AImpl_EnumGroups,
    IDirectPlay3AImpl_EnumPlayers,
    IDirectPlay3AImpl_EnumSessions,
    IDirectPlay3AImpl_GetCaps,
    IDirectPlay3AImpl_GetGroupData,
    IDirectPlay3AImpl_GetGroupName,
    IDirectPlay3AImpl_GetMessageCount,
    IDirectPlay3AImpl_GetPlayerAddress,
    IDirectPlay3AImpl_GetPlayerCaps,
    IDirectPlay3AImpl_GetPlayerData,
    IDirectPlay3AImpl_GetPlayerName,
    IDirectPlay3AImpl_GetSessionDesc,
    IDirectPlay3AImpl_Initialize,
    IDirectPlay3AImpl_Open,
    IDirectPlay3AImpl_Receive,
    IDirectPlay3AImpl_Send,
    IDirectPlay3AImpl_SetGroupData,
    IDirectPlay3AImpl_SetGroupName,
    IDirectPlay3AImpl_SetPlayerData,
    IDirectPlay3AImpl_SetPlayerName,
    IDirectPlay3AImpl_SetSessionDesc,
    IDirectPlay3AImpl_AddGroupToGroup,
    IDirectPlay3AImpl_CreateGroupInGroup,
    IDirectPlay3AImpl_DeleteGroupFromGroup,
    IDirectPlay3AImpl_EnumConnections,
    IDirectPlay3AImpl_EnumGroupsInGroup,
    IDirectPlay3AImpl_GetGroupConnectionSettings,
    IDirectPlay3AImpl_InitializeConnection,
    IDirectPlay3AImpl_SecureOpen,
    IDirectPlay3AImpl_SendChatMessage,
    IDirectPlay3AImpl_SetGroupConnectionSettings,
    IDirectPlay3AImpl_StartSession,
    IDirectPlay3AImpl_GetGroupFlags,
    IDirectPlay3AImpl_GetGroupParent,
    IDirectPlay3AImpl_GetPlayerAccount,
    IDirectPlay3AImpl_GetPlayerFlags
};

static const IDirectPlay4Vtbl dp4_vt =
{
    IDirectPlay4Impl_QueryInterface,
    IDirectPlay4Impl_AddRef,
    IDirectPlay4Impl_Release,
    IDirectPlay4Impl_AddPlayerToGroup,
    IDirectPlay4Impl_Close,
    IDirectPlay4Impl_CreateGroup,
    IDirectPlay4Impl_CreatePlayer,
    IDirectPlay4Impl_DeletePlayerFromGroup,
    IDirectPlay4Impl_DestroyGroup,
    IDirectPlay4Impl_DestroyPlayer,
    IDirectPlay4Impl_EnumGroupPlayers,
    IDirectPlay4Impl_EnumGroups,
    IDirectPlay4Impl_EnumPlayers,
    IDirectPlay4Impl_EnumSessions,
    IDirectPlay4Impl_GetCaps,
    IDirectPlay4Impl_GetGroupData,
    IDirectPlay4Impl_GetGroupName,
    IDirectPlay4Impl_GetMessageCount,
    IDirectPlay4Impl_GetPlayerAddress,
    IDirectPlay4Impl_GetPlayerCaps,
    IDirectPlay4Impl_GetPlayerData,
    IDirectPlay4Impl_GetPlayerName,
    IDirectPlay4Impl_GetSessionDesc,
    IDirectPlay4Impl_Initialize,
    IDirectPlay4Impl_Open,
    IDirectPlay4Impl_Receive,
    IDirectPlay4Impl_Send,
    IDirectPlay4Impl_SetGroupData,
    IDirectPlay4Impl_SetGroupName,
    IDirectPlay4Impl_SetPlayerData,
    IDirectPlay4Impl_SetPlayerName,
    IDirectPlay4Impl_SetSessionDesc,
    IDirectPlay4Impl_AddGroupToGroup,
    IDirectPlay4Impl_CreateGroupInGroup,
    IDirectPlay4Impl_DeleteGroupFromGroup,
    IDirectPlay4Impl_EnumConnections,
    IDirectPlay4Impl_EnumGroupsInGroup,
    IDirectPlay4Impl_GetGroupConnectionSettings,
    IDirectPlay4Impl_InitializeConnection,
    IDirectPlay4Impl_SecureOpen,
    IDirectPlay4Impl_SendChatMessage,
    IDirectPlay4Impl_SetGroupConnectionSettings,
    IDirectPlay4Impl_StartSession,
    IDirectPlay4Impl_GetGroupFlags,
    IDirectPlay4Impl_GetGroupParent,
    IDirectPlay4Impl_GetPlayerAccount,
    IDirectPlay4Impl_GetPlayerFlags,
    IDirectPlay4Impl_GetGroupOwner,
    IDirectPlay4Impl_SetGroupOwner,
    IDirectPlay4Impl_SendEx,
    IDirectPlay4Impl_GetMessageQueue,
    IDirectPlay4Impl_CancelMessage,
    IDirectPlay4Impl_CancelPriority
};

static const IDirectPlay4Vtbl dp4A_vt =
{
    IDirectPlay4AImpl_QueryInterface,
    IDirectPlay4AImpl_AddRef,
    IDirectPlay4AImpl_Release,
    IDirectPlay4AImpl_AddPlayerToGroup,
    IDirectPlay4AImpl_Close,
    IDirectPlay4AImpl_CreateGroup,
    IDirectPlay4AImpl_CreatePlayer,
    IDirectPlay4AImpl_DeletePlayerFromGroup,
    IDirectPlay4AImpl_DestroyGroup,
    IDirectPlay4AImpl_DestroyPlayer,
    IDirectPlay4AImpl_EnumGroupPlayers,
    IDirectPlay4AImpl_EnumGroups,
    IDirectPlay4AImpl_EnumPlayers,
    IDirectPlay4AImpl_EnumSessions,
    IDirectPlay4AImpl_GetCaps,
    IDirectPlay4AImpl_GetGroupData,
    IDirectPlay4AImpl_GetGroupName,
    IDirectPlay4AImpl_GetMessageCount,
    IDirectPlay4AImpl_GetPlayerAddress,
    IDirectPlay4AImpl_GetPlayerCaps,
    IDirectPlay4AImpl_GetPlayerData,
    IDirectPlay4AImpl_GetPlayerName,
    IDirectPlay4AImpl_GetSessionDesc,
    IDirectPlay4AImpl_Initialize,
    IDirectPlay4AImpl_Open,
    IDirectPlay4AImpl_Receive,
    IDirectPlay4AImpl_Send,
    IDirectPlay4AImpl_SetGroupData,
    IDirectPlay4AImpl_SetGroupName,
    IDirectPlay4AImpl_SetPlayerData,
    IDirectPlay4AImpl_SetPlayerName,
    IDirectPlay4AImpl_SetSessionDesc,
    IDirectPlay4AImpl_AddGroupToGroup,
    IDirectPlay4AImpl_CreateGroupInGroup,
    IDirectPlay4AImpl_DeleteGroupFromGroup,
    IDirectPlay4AImpl_EnumConnections,
    IDirectPlay4AImpl_EnumGroupsInGroup,
    IDirectPlay4AImpl_GetGroupConnectionSettings,
    IDirectPlay4AImpl_InitializeConnection,
    IDirectPlay4AImpl_SecureOpen,
    IDirectPlay4AImpl_SendChatMessage,
    IDirectPlay4AImpl_SetGroupConnectionSettings,
    IDirectPlay4AImpl_StartSession,
    IDirectPlay4AImpl_GetGroupFlags,
    IDirectPlay4AImpl_GetGroupParent,
    IDirectPlay4AImpl_GetPlayerAccount,
    IDirectPlay4AImpl_GetPlayerFlags,
    IDirectPlay4AImpl_GetGroupOwner,
    IDirectPlay4AImpl_SetGroupOwner,
    IDirectPlay4AImpl_SendEx,
    IDirectPlay4AImpl_GetMessageQueue,
    IDirectPlay4AImpl_CancelMessage,
    IDirectPlay4AImpl_CancelPriority
};

HRESULT dplay_create( REFIID riid, void **ppv )
{
    IDirectPlayImpl *obj;
    HRESULT hr;

    TRACE( "(%s, %p)\n", debugstr_guid( riid ), ppv );

    *ppv = NULL;
    obj = malloc( sizeof( *obj ) );
    if ( !obj )
        return DPERR_OUTOFMEMORY;

    obj->IDirectPlay_iface.lpVtbl = &dp_vt;
    obj->IDirectPlay2A_iface.lpVtbl = &dp2A_vt;
    obj->IDirectPlay2_iface.lpVtbl = &dp2_vt;
    obj->IDirectPlay3A_iface.lpVtbl = &dp3A_vt;
    obj->IDirectPlay3_iface.lpVtbl = &dp3_vt;
    obj->IDirectPlay4A_iface.lpVtbl = &dp4A_vt;
    obj->IDirectPlay4_iface.lpVtbl = &dp4_vt;
    obj->numIfaces = 1;
    obj->ref = 0;
    obj->ref2A = 0;
    obj->ref2 = 0;
    obj->ref3A = 0;
    obj->ref3 = 0;
    obj->ref4A = 0;
    obj->ref4 = 1;

    InitializeCriticalSection( &obj->lock );
    obj->lock.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": IDirectPlayImpl.lock");

    if ( DP_CreateDirectPlay2( obj ) )
        hr = IDirectPlayX_QueryInterface( &obj->IDirectPlay4_iface, riid, ppv );
    else
        hr = DPERR_NOMEMORY;
    IDirectPlayX_Release( &obj->IDirectPlay4_iface );

    return hr;
}


HRESULT DP_GetSPPlayerData( IDirectPlayImpl *lpDP, DPID idPlayer, void **lplpData )
{
  lpPlayerList lpPlayer = DP_FindPlayer( lpDP, idPlayer );

  if( lpPlayer == NULL )
  {
    return DPERR_INVALIDPLAYER;
  }

  *lplpData = lpPlayer->lpPData->lpSPPlayerData;

  return DP_OK;
}

HRESULT DP_SetSPPlayerData( IDirectPlayImpl *lpDP, DPID idPlayer, void *lpData )
{
  lpPlayerList lpPlayer = DP_FindPlayer( lpDP, idPlayer );

  if( lpPlayer == NULL )
  {
    return DPERR_INVALIDPLAYER;
  }

  lpPlayer->lpPData->lpSPPlayerData = lpData;

  return DP_OK;
}

/***************************************************************************
 *  DirectPlayEnumerateAW
 *
 *  The pointer to the structure lpContext will be filled with the
 *  appropriate data for each service offered by the OS. These services are
 *  not necessarily available on this particular machine but are defined
 *  as simple service providers under the "Service Providers" registry key.
 *  This structure is then passed to lpEnumCallback for each of the different
 *  services.
 *
 *  This API is useful only for applications written using DirectX3 or
 *  worse. It is superseded by IDirectPlay3::EnumConnections which also
 *  gives information on the actual connections.
 *
 * defn of a service provider:
 * A dynamic-link library used by DirectPlay to communicate over a network.
 * The service provider contains all the network-specific code required
 * to send and receive messages. Online services and network operators can
 * supply service providers to use specialized hardware, protocols, communications
 * media, and network resources.
 *
 */
static HRESULT DirectPlayEnumerateAW(LPDPENUMDPCALLBACKA lpEnumCallbackA,
                                     LPDPENUMDPCALLBACKW lpEnumCallbackW,
                                     LPVOID lpContext)
{
    HKEY   hkResult;
    DWORD  dwIndex;
    FILETIME filetime;

    char  *descriptionA = NULL;
    DWORD max_sizeOfDescriptionA = 0;
    WCHAR *descriptionW = NULL;
    DWORD max_sizeOfDescriptionW = 0;
    DWORD sizeOfSubKeyName;
    WCHAR subKeyName[255]; /* 255 is the maximum key size according to MSDN */
    LONG  ret_value;
    static GUID *guid_cache;
    static int cache_count;
    
    if (!lpEnumCallbackA && !lpEnumCallbackW)
    {
	return DPERR_INVALIDPARAMS;
    }
    
    /* Need to loop over the service providers in the registry */
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\DirectPlay\\Service Providers",
		      0, KEY_READ, &hkResult) != ERROR_SUCCESS)
    {
	/* Hmmm. Does this mean that there are no service providers? */
	ERR(": no service provider key in the registry - check your Wine installation !!!\n");
	return DPERR_GENERIC;
    }

    dwIndex = 0;
    do
    {
        sizeOfSubKeyName = ARRAY_SIZE(subKeyName);
	ret_value = RegEnumKeyW(hkResult, dwIndex, subKeyName, sizeOfSubKeyName);
	dwIndex++;
    }
    while (ret_value == ERROR_SUCCESS);
    /* The game Swing from bug 37185 expects GUID values to persist after
     * the end of the enumeration. */
    if (cache_count < dwIndex)
    {
	free(guid_cache);
	guid_cache = malloc(sizeof(GUID) * dwIndex);
	if (!guid_cache)
	{
	    ERR(": failed to allocate required memory.\n");
	    return DPERR_EXCEPTION;
	}
	cache_count = dwIndex;
    }
    /* Traverse all the service providers we have available */
    dwIndex = 0;
    while (1)
    {
	HKEY  hkServiceProvider;
	WCHAR guidKeyContent[(2 * 16) + 1 + 6 /* This corresponds to '{....-..-..-..-......}' */ ];
	DWORD sizeOfGuidKeyContent = sizeof(guidKeyContent);

        sizeOfSubKeyName = ARRAY_SIZE(subKeyName);
	ret_value = RegEnumKeyExW(hkResult, dwIndex, subKeyName, &sizeOfSubKeyName,
				  NULL, NULL, NULL, &filetime);
	if (ret_value == ERROR_NO_MORE_ITEMS)
	    break;
	else if (ret_value != ERROR_SUCCESS)
	{
	    ERR(": could not enumerate on service provider key.\n");
	    return DPERR_EXCEPTION;
	}
	TRACE(" this time through sub-key %s.\n", debugstr_w(subKeyName));
	
	/* Open the key for this service provider */
	if (RegOpenKeyExW(hkResult, subKeyName, 0, KEY_READ, &hkServiceProvider) != ERROR_SUCCESS)
	{
	    ERR(": could not open registry key for service provider %s.\n", debugstr_w(subKeyName));
	    continue;
	}
	
	/* Get the GUID from the registry */
        if (RegQueryValueExW(hkServiceProvider, L"Guid",
			     NULL, NULL, (LPBYTE) guidKeyContent, &sizeOfGuidKeyContent) != ERROR_SUCCESS)
	{
	    ERR(": missing GUID registry data member for service provider %s.\n", debugstr_w(subKeyName));
	    continue;
	}
	if (sizeOfGuidKeyContent != sizeof(guidKeyContent))
	{
	    ERR(": invalid format for the GUID registry data member for service provider %s (%s).\n", debugstr_w(subKeyName), debugstr_w(guidKeyContent));
	    continue;
	}
	CLSIDFromString(guidKeyContent, &guid_cache[dwIndex]);
	
	/* The enumeration will return FALSE if we are not to continue.
	 *
	 * Note: on my windows box, major / minor version is 6 / 0 for all service providers
	 *       and have no relation to any of the two dwReserved1 and dwReserved2 keys.
	 *       I think that it simply means that they are in-line with DirectX 6.0
	 */
	if (lpEnumCallbackA)
	{
	    DWORD sizeOfDescription = 0;
	    
	    /* Note that this is the A case of this function, so use the A variant to get the description string */
	    if (RegQueryValueExA(hkServiceProvider, "DescriptionA",
				 NULL, NULL, NULL, &sizeOfDescription) != ERROR_SUCCESS)
	    {
		ERR(": missing 'DescriptionA' registry data member for service provider %s.\n", debugstr_w(subKeyName));
		continue;
	    }
	    if (sizeOfDescription > max_sizeOfDescriptionA)
	    {
		free(descriptionA);
		max_sizeOfDescriptionA = sizeOfDescription;
	    }
	    descriptionA = malloc(sizeOfDescription);
	    RegQueryValueExA(hkServiceProvider, "DescriptionA",
			     NULL, NULL, (LPBYTE) descriptionA, &sizeOfDescription);
	    
	    if (!lpEnumCallbackA(&guid_cache[dwIndex], descriptionA, 6, 0, lpContext))
		goto end;
	}
	else
	{
	    DWORD sizeOfDescription = 0;

            if (RegQueryValueExW(hkServiceProvider, L"DescriptionW",
				 NULL, NULL, NULL, &sizeOfDescription) != ERROR_SUCCESS)
	    {
		ERR(": missing 'DescriptionW' registry data member for service provider %s.\n", debugstr_w(subKeyName));
		continue;
	    }
	    if (sizeOfDescription > max_sizeOfDescriptionW)
	    {
		free(descriptionW);
		max_sizeOfDescriptionW = sizeOfDescription;
	    }
	    descriptionW = malloc(sizeOfDescription);
            RegQueryValueExW(hkServiceProvider, L"DescriptionW",
			     NULL, NULL, (LPBYTE) descriptionW, &sizeOfDescription);

	    if (!lpEnumCallbackW(&guid_cache[dwIndex], descriptionW, 6, 0, lpContext))
		goto end;
	}
      
      dwIndex++;
  }

 end:
    free(descriptionA);
    free(descriptionW);

    return DP_OK;
}

/***************************************************************************
 *  DirectPlayEnumerate  [DPLAYX.9]
 *  DirectPlayEnumerateA [DPLAYX.2]
 */
HRESULT WINAPI DirectPlayEnumerateA(LPDPENUMDPCALLBACKA lpEnumCallback, LPVOID lpContext )
{
    TRACE("(%p,%p)\n", lpEnumCallback, lpContext);
    
    return DirectPlayEnumerateAW(lpEnumCallback, NULL, lpContext);
}

/***************************************************************************
 *  DirectPlayEnumerateW [DPLAYX.3]
 */
HRESULT WINAPI DirectPlayEnumerateW(LPDPENUMDPCALLBACKW lpEnumCallback, LPVOID lpContext )
{
    TRACE("(%p,%p)\n", lpEnumCallback, lpContext);
    
    return DirectPlayEnumerateAW(NULL, lpEnumCallback, lpContext);
}

typedef struct tagCreateEnum
{
  LPVOID  lpConn;
  LPCGUID lpGuid;
} CreateEnumData, *lpCreateEnumData;

/* Find and copy the matching connection for the SP guid */
static BOOL CALLBACK cbDPCreateEnumConnections(
    LPCGUID     lpguidSP,
    LPVOID      lpConnection,
    DWORD       dwConnectionSize,
    LPCDPNAME   lpName,
    DWORD       dwFlags,
    LPVOID      lpContext)
{
  lpCreateEnumData lpData = (lpCreateEnumData)lpContext;

  if( IsEqualGUID( lpguidSP, lpData->lpGuid ) )
  {
    TRACE( "Found SP entry with guid %s\n", debugstr_guid(lpData->lpGuid) );

    lpData->lpConn = malloc( dwConnectionSize );
    CopyMemory( lpData->lpConn, lpConnection, dwConnectionSize );

    /* Found the record that we were looking for */
    return FALSE;
  }

  /* Haven't found what were looking for yet */
  return TRUE;
}


/***************************************************************************
 *  DirectPlayCreate [DPLAYX.1]
 *
 */
HRESULT WINAPI DirectPlayCreate
( LPGUID lpGUID, LPDIRECTPLAY *lplpDP, IUnknown *pUnk )
{
  HRESULT hr;
  LPDIRECTPLAY3A lpDP3A;
  CreateEnumData cbData;

  TRACE( "lpGUID=%s lplpDP=%p pUnk=%p\n", debugstr_guid(lpGUID), lplpDP, pUnk );

  if( pUnk != NULL )
  {
    return CLASS_E_NOAGGREGATION;
  }

  if( (lplpDP == NULL) || (lpGUID == NULL) )
  {
    return DPERR_INVALIDPARAMS;
  }

  if ( dplay_create( &IID_IDirectPlay, (void**)lplpDP ) != DP_OK )
    return DPERR_UNAVAILABLE;

  if( IsEqualGUID( &GUID_NULL, lpGUID ) )
  {
    /* The GUID_NULL means don't bind a service provider. Just return the
       interface as is */
    return DP_OK;
  }

  /* Bind the desired service provider since lpGUID is non NULL */
  TRACE( "Service Provider binding for %s\n", debugstr_guid(lpGUID) );

  /* We're going to use a DP3 interface */
  hr = IDirectPlayX_QueryInterface( *lplpDP, &IID_IDirectPlay3A,
                                    (LPVOID*)&lpDP3A );
  if( FAILED(hr) )
  {
    ERR( "Failed to get DP3 interface: %s\n", DPLAYX_HresultToString(hr) );
    return hr;
  }

  cbData.lpConn = NULL;
  cbData.lpGuid = lpGUID;

  /* We were given a service provider, find info about it... */
  hr = IDirectPlayX_EnumConnections( lpDP3A, NULL, cbDPCreateEnumConnections,
                                     &cbData, DPCONNECTION_DIRECTPLAY );
  if( ( FAILED(hr) ) ||
      ( cbData.lpConn == NULL )
    )
  {
    ERR( "Failed to get Enum for SP: %s\n", DPLAYX_HresultToString(hr) );
    IDirectPlayX_Release( lpDP3A );
    return DPERR_UNAVAILABLE;
  }

  /* Initialize the service provider */
  hr = IDirectPlayX_InitializeConnection( lpDP3A, cbData.lpConn, 0 );
  if( FAILED(hr) )
  {
    ERR( "Failed to Initialize SP: %s\n", DPLAYX_HresultToString(hr) );
    free( cbData.lpConn );
    IDirectPlayX_Release( lpDP3A );
    return hr;
  }

  /* Release our version of the interface now that we're done with it */
  IDirectPlayX_Release( lpDP3A );
  free( cbData.lpConn );

  return DP_OK;
}
