// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	CookOnTheFlyServer.h : handles polite cook requests via network ;)
=============================================================================*/

#pragma once

#include "CookOnTheFlyServer.generated.h"


class FChunkManifestGenerator;

enum class ECookInitializationFlags
{
	None = 0x0,						// No flags
	Compressed = 0x1,				// will save compressed packages
	Iterative = 0x2,				// use iterative cooking (previous cooks will not be cleaned unless detected out of date, experimental)
	SkipEditorContent = 0x4,		// do not cook any content in the content\editor directory
	Unversioned = 0x8,				// save the cooked packages without a version number
	AutoTick = 0x10,				// enable ticking (only works in the editor)
	AsyncSave = 0x20,				// save packages async
	LeakTest = 0x40,				// test for uobject leaks after each level load
	IncludeServerMaps = 0x80,		// should we include the server maps when cooking
	GenerateStreamingInstallManifest = 0x100,  // should we generate streaming install manifest
};
ENUM_CLASS_FLAGS(ECookInitializationFlags);

UENUM()
namespace ECookMode
{
	enum Type
	{
		CookOnTheFly,				// default mode, handles requests from network
		CookByTheBookFromTheEditor,	// precook all resources while in the editor
		CookByTheBook,				// cooking by the book (not in the editor)
	};
}

UCLASS()
class UCookOnTheFlyServer : public UObject, public FTickableEditorObject
{
	GENERATED_UCLASS_BODY()
private:

	/** Array which has been made thread safe :) */
	template<typename Type, typename SynchronizationObjectType, typename ScopeLockType>
	struct FUnsynchronizedQueue
	{
	private:
		mutable SynchronizationObjectType	SynchronizationObject; // made this mutable so this class can have const functions and still be thread safe
		TArray<Type>		Items;
	public:
		void Enqueue(const Type& Item)
		{
			ScopeLockType ScopeLock(&SynchronizationObject);
			Items.Add(Item);
		}
		void EnqueueUnique( const Type& Item )
		{
			ScopeLockType ScopeLock(&SynchronizationObject);
			Items.AddUnique(Item);
		}
		bool Dequeue(Type* Result)
		{
			ScopeLockType ScopeLock(&SynchronizationObject);
			if (Items.Num())
			{
				*Result = Items[0];
				Items.RemoveAt(0);
				return true;
			}
			return false;
		}
		void DequeueAll(TArray<Type>& Results)
		{
			ScopeLockType ScopeLock(&SynchronizationObject);
			Results += Items;
			Items.Empty();
		}

		bool HasItems() const
		{
			ScopeLockType ScopeLock( &SynchronizationObject );
			return Items.Num() > 0;
		}

		void Remove( const Type& Item ) 
		{
			ScopeLockType ScopeLock( &SynchronizationObject );
			Items.Remove( Item );
		}

		void CopyItems( TArray<Type> &InItems ) const
		{
			ScopeLockType ScopeLock( &SynchronizationObject );
			InItems = Items;
		}

		int Num() const 
		{
			ScopeLockType ScopeLock( &SynchronizationObject );
			return Items.Num();
		}

		void Empty() 
		{
			ScopeLockType ScopeLock( &SynchronizationObject );
			Items.Empty();
		}
	};



	struct FDummyCriticalSection
	{
	public:
		FORCEINLINE void Lock() { }
		FORCEINLINE void Unlock() { }
	};

	struct FDummyScopeLock
	{
	public:
		FDummyScopeLock( FDummyCriticalSection * ) { }
	};

public:


	template<typename Type>
	struct FThreadSafeQueue : public FUnsynchronizedQueue<Type, FCriticalSection, FScopeLock>
	{
		/**
		 * Don't add any functions here, this is just a overqualified typedef
		 * Add functions / functionality to the FUnsynchronizedQueue
		 */
	};
	
	template<typename Type>
	struct FQueue : public FUnsynchronizedQueue<Type, FDummyCriticalSection, FDummyScopeLock>
	{
		/**
		 * Don't add any functions here, this is just a overqualified typedef
		 * Add functions / functionality to the FUnsynchronizedQueue
		 */
	};

	// pending delete: don't think I need this anymore
	/*template<typename Type>
	struct FLookupQueue
	{
	private:
		TSet<Type> Set;
		FQueue<Type> Queue;

	public:
		void Enqueue(const Type& Item)
		{
			Set.Add(Item);
			Queue.Enqueue(Item);
		}
		void EnqueueUnique( const Type& Item )
		{
			if ( Set.Find(Item) == NULL )
			{
				Enqueue( Item );
			}
		}

		bool Dequeue(Type* Result)
		{
			if (Queue.Num())
			{
				Queue.Dequeue(Result);
				Set.Remove(*Result);
				return true;
			}
			return false;
		}
		void DequeueAll(TArray<Type>& Results)
		{
			Queue.DequeueAll(Results);
			Set.Empty();
		}

		bool HasItems() const
		{
			return Queue.Num() > 0;
		}

		void Remove( const Type& Item ) 
		{
			Queue.Remove( Item );
			Set.Remove(Item);
		}

		int Num() const 
		{
			return Queue.Num();
		}

	};*/

public:
	/** cooked file requests which includes platform which file is requested for */
	struct FFilePlatformRequest
	{
	private:
		FName Filename;
		TArray<FName> Platformnames;
	public:
		
		// yes we have some friends
		friend uint32 GetTypeHash(const UCookOnTheFlyServer::FFilePlatformRequest& Key);

		FFilePlatformRequest() { }

		FFilePlatformRequest( const FName& InFilename, const TArray<FName>& InPlatformname ) : Filename( InFilename )
		{
			Platformnames = InPlatformname;
		}

		FFilePlatformRequest( const FName& InFilename, TArray<FName>&& InPlatformname ) : Filename( InFilename ), Platformnames(MoveTemp(InPlatformname))
		{
		}

		FFilePlatformRequest( const FFilePlatformRequest& InFilePlatformRequest ) : Filename( InFilePlatformRequest.Filename ), Platformnames( InFilePlatformRequest.Platformnames )
		{
		}
		
		FFilePlatformRequest( FFilePlatformRequest&& InFilePlatformRequest ) : Filename( MoveTemp(InFilePlatformRequest.Filename) ), Platformnames( MoveTemp(InFilePlatformRequest.Platformnames) )
		{
		}
		

		void SetFilename( const FString &InFilename ) 
		{
			Filename = FName(*InFilename);
		}

		const FName &GetFilename() const
		{
			return Filename;
		}

		const TArray<FName> &GetPlatformnames() const
		{
			return Platformnames;
		}

		void RemovePlatform( const FName &Platform )
		{
			Platformnames.Remove(Platform);
		}

		void AddPlatform( const FName &Platform )
		{
			check( Platform != NAME_None );
			Platformnames.Add(Platform );
		}

		bool HasPlatform( const FName &Platform ) const
		{
			return Platformnames.Find(Platform) != INDEX_NONE;
		}

		bool IsValid()  const
		{
			return Filename != NAME_None;
		}

		void Clear()
		{
			Filename = TEXT("");
			Platformnames.Empty();
		}

		FFilePlatformRequest &operator=( FFilePlatformRequest &&InFileRequest )
		{
			Filename = MoveTemp( InFileRequest.Filename );
			Platformnames = MoveTemp( InFileRequest.Platformnames );
			return *this;
		}

		bool operator ==( const FFilePlatformRequest &InFileRequest ) const
		{
			if ( InFileRequest.Filename == Filename )
			{
				if ( InFileRequest.Platformnames == Platformnames )
				{
					return true;
				}
			}
			return false;
		}

		FORCEINLINE FString &&ToString() const
		{
			FString Result = FString::Printf(TEXT("%s;"), *Filename.ToString());

			for ( const auto &Platform : Platformnames )
			{
				Result += FString::Printf(TEXT("%s,"), *Platform.ToString() );
			}
			return MoveTemp(Result);
		}

	};


private:

	/** Helper list of all files which have been cooked */
	struct FThreadSafeFilenameSet
	{
	private:
		mutable FCriticalSection	SynchronizationObject;
		TMap<FName, FFilePlatformRequest> FilesProcessed;
	public:

		void Lock()
		{
			SynchronizationObject.Lock();
		}
		void Unlock()
		{
			SynchronizationObject.Unlock();
		}

		void Add(const FFilePlatformRequest& Request)
		{
			FScopeLock ScopeLock(&SynchronizationObject);
			check(Request.IsValid());

			// see if it's already in the requests list
			FFilePlatformRequest *ExistingRequest = FilesProcessed.Find(Request.GetFilename() );
			
			if ( ExistingRequest )
			{
				check( ExistingRequest->GetFilename() == Request.GetFilename() );
				for ( const auto &Platform : Request.GetPlatformnames() )
				{
					ExistingRequest->AddPlatform(Platform);
				}
			}
			else
				FilesProcessed.Add(Request.GetFilename(), Request);
		}
		bool Exists(const FFilePlatformRequest& Request) const
		{
			FScopeLock ScopeLock(&SynchronizationObject);

			const FFilePlatformRequest* OurRequest = FilesProcessed.Find( Request.GetFilename() );
			
			if (!OurRequest)
			{
				return false;
			}

			// make sure all the platforms are completed
			for ( const auto& Platform : Request.GetPlatformnames() )
			{
				if ( !OurRequest->GetPlatformnames().Contains( Platform ) )
				{
					return false;
				}
			}

			return true;
			// return FilesProcessed.Contains(Filename);
		}
		// two versions of this function so I don't have to create temporary FFIleplatformRequest in some cases to call the exists function
		bool Exists( const FName& Filename, const TArray<FName>& PlatformNames )
		{
			FScopeLock ScopeLock(&SynchronizationObject);

			const FFilePlatformRequest* OurRequest = FilesProcessed.Find( Filename );

			if (!OurRequest)
			{
				return false;
			}

			// make sure all the platforms are completed
			for ( const auto& Platform : PlatformNames )
			{
				if ( !OurRequest->GetPlatformnames().Contains( Platform ) )
				{
					return false;
				}
			}

			return true;
		}

		bool GetCookedPlatforms( const FName& Filename, TArray<FName>& PlatformList )
		{
			FScopeLock ScopeLock( &SynchronizationObject );
			const FFilePlatformRequest* Request = FilesProcessed.Find(Filename);
			if ( Request )
			{
				PlatformList = Request->GetPlatformnames();
				return true;
			}
			return false;
			
		}
		int RemoveAll( const FName& Filename )
		{
			FScopeLock ScopeLock( &SynchronizationObject );
			return FilesProcessed.Remove( Filename );
		}
		void Empty()
		{
			FScopeLock ScopeLock( &SynchronizationObject );
			FilesProcessed.Empty();
		}
	};


	struct FFilenameQueue
	{
	private:
		TArray<FName>			Queue;
		TMap<FName, TArray<FName>> PlatformList;
		mutable FCriticalSection SynchronizationObject;
	public:
		void EnqueueUnique(const FFilePlatformRequest& Request, bool ForceEnqueFront = false)
		{
			FScopeLock ScopeLock( &SynchronizationObject );
			TArray<FName>* Platforms = PlatformList.Find(Request.GetFilename());
			if ( Platforms == NULL )
			{
				PlatformList.Add(Request.GetFilename(), Request.GetPlatformnames());
				Queue.Add(Request.GetFilename());
			}
			else
			{
				// add the requested platforms to the platform list
				for ( const auto& Platform : Request.GetPlatformnames() )
				{
					Platforms->AddUnique(Platform);
				}
			}

			if ( ForceEnqueFront )
			{
				int32 Index = Queue.Find(Request.GetFilename());
				check( Index != INDEX_NONE );
				if ( Index != 0 )
				{
					Queue.Swap(0, Index);
				}
			}
		}

		bool Dequeue(FFilePlatformRequest* Result)
		{
			FScopeLock ScopeLock( &SynchronizationObject );
			if (Queue.Num())
			{
				FName Filename = Queue[0];
				Queue.RemoveAt(0);
				TArray<FName> Platforms = PlatformList.FindChecked(Filename);
				PlatformList.Remove(Filename);
				*Result = MoveTemp(FFilePlatformRequest(MoveTemp(Filename), MoveTemp(Platforms)));
				return true;
			}
			return false;
		}

		void DequeueAllRequests( TArray<FFilePlatformRequest>& RequestArray )
		{
			FScopeLock ScopeLock( &SynchronizationObject );
			if ( Queue.Num() )
			{
				for ( const auto& Request : PlatformList )
				{
					RequestArray.Add( FFilePlatformRequest( Request.Key, Request.Value ) );
				}
				PlatformList.Empty();
				Queue.Empty();
			}
		}

		bool Exists( const FName& Filename, const TArray<FName>& PlatformNames ) const 
		{
			const TArray<FName>* Platforms = PlatformList.Find( Filename );
			if ( Platforms == NULL )
				return false;

			for ( const auto& PlatformName : PlatformNames )
			{
				if ( !Platforms->Contains( PlatformName ) )
					return false;
			}
			return true;
		}
		
		bool HasItems() const
		{
			FScopeLock ScopeLock( &SynchronizationObject );
			return Queue.Num() > 0;
		}

		int Num() const 
		{
			FScopeLock ScopeLock( &SynchronizationObject );
			return Queue.Num();
		}


		void Empty()  
		{
			FScopeLock ScopeLock( &SynchronizationObject );
			Queue.Empty();
			PlatformList.Empty();
		}
	};

	struct FThreadSafeUnsolicitedPackagesList
	{
	private:
		FCriticalSection SyncObject;
		TArray<FFilePlatformRequest> CookedPackages;
	public:
		void AddCookedPackage( const FFilePlatformRequest& PlatformRequest )
		{
			FScopeLock S( &SyncObject );
			CookedPackages.Add( PlatformRequest );
		}
		void GetPackagesForPlatformAndRemove( const FName& Platform, TArray<FName> PackageNames )
		{
			FScopeLock S( &SyncObject );
			
			for ( int I = CookedPackages.Num()-1; I >= 0; --I )
			{
				FFilePlatformRequest &Request = CookedPackages[I];

				if ( Request.GetPlatformnames().Contains( Platform ) )
				{
					// remove the platform
					Request.RemovePlatform( Platform );

					if ( Request.GetPlatformnames().Num() == 0 )
					{
						CookedPackages.RemoveAt(I);
					}
				}
			}
		}
		void Empty()
		{
			FScopeLock S( &SyncObject );
			CookedPackages.Empty();
		}
	};
private:
	/** Current cook mode the cook on the fly server is running in */
	ECookMode::Type CurrentCookMode;
	
	//////////////////////////////////////////////////////////////////////////
	// Cook by the book options
	struct FCookByTheBookOptions
	{
	public:
		FCookByTheBookOptions() : bGenerateStreamingInstallManifests(false),
			bRunning(false),
			CookTime( 0.0 ),
			CookStartTime( 0.0 )
		{ }

		/** Should we generate streaming install manifests (only valid option in cook by the book) */
		bool bGenerateStreamingInstallManifests;
		/** Is cook by the book currently running */
		bool bRunning;
		/** Cancel has been queued will be processed next tick */
		bool bCancel;
		/** Leak test: last gc items (only valid when running from commandlet requires gc between each cooked package) */
		TSet<FWeakObjectPtr> LastGCItems;
		/** Map of platform name to manifest generator, manifest is only used in cook by the book however it needs to be maintained across multiple cook by the books. */
		TMap<FName, FChunkManifestGenerator*> ManifestGenerators;
		/** If a cook is cancelled next cook will need to resume cooking */ 
		TArray<FFilePlatformRequest> PreviousCookRequests; 
		double CookTime;
		double CookStartTime;
	};
	FCookByTheBookOptions* CookByTheBookOptions;
	

	//////////////////////////////////////////////////////////////////////////
	// Cook on the fly options
	/** Cook on the fly server uses the NetworkFileServer */
	TArray<class INetworkFileServer*> NetworkFileServers;

	//////////////////////////////////////////////////////////////////////////
	// General cook options
	ECookInitializationFlags CookFlags;
	TAutoPtr<class FSandboxPlatformFile> SandboxFile;
	bool bIsSavingPackage; // used to stop recursive mark package dirty functions


	FThreadSafeQueue<struct FRecompileRequest*> RecompileRequests;
	FFilenameQueue CookRequests; // list of requested files
	FThreadSafeUnsolicitedPackagesList UnsolicitedCookedPackages;
	FThreadSafeFilenameSet CookedPackages; // set of files which have been cooked when needing to recook a file the entry will need to be removed from here

public:


	

	enum ECookOnTheSideResult
	{
		COSR_CookedMap			= 0x00000001,
		COSR_CookedPackage		= 0x00000002,
		COSR_ErrorLoadingPackage= 0x00000004,
	};


	virtual ~UCookOnTheFlyServer();

	/**
	 * FTickableEditorObject interface used by cook on the side
	 */
	TStatId GetStatId() const override;
	void Tick(float DeltaTime) override;
	bool IsTickable() const override;
	ECookMode::Type GetCookMode() const { return CurrentCookMode; }

	/**
	 * Initialize the CookServer so that either CookOnTheFly can be called or Cook on the side can be started and ticked
	 */
	void Initialize( ECookMode::Type DesiredCookMode, ECookInitializationFlags InCookInitializationFlags, const FString &OutputDirectoryOverride = FString(TEXT("")) );

	/**
	 * Cook on the side, cooks while also running the editor...
	 *
	 * @param  BindAnyPort					Whether to bind on any port or the default port.
	 *
	 * @return true on success, false otherwise.
	 */
	bool StartNetworkFileServer( bool BindAnyPort );

	/**
	 * Broadcast our the fileserver presence on the network
	 */
	bool BroadcastFileserverPresence( const FGuid &InstanceId );
	/** 
	 * Stop the network file server
	 *
	 */
	void EndNetworkFileServer();


	/**
	 * Start a cook by the book session
	 * Cook on the fly can't run at the same time as cook by the book
	 */
	void StartCookByTheBook(const TArray<ITargetPlatform*>& TargetPlatforms, const TArray<FString>& CookMaps, const TArray<FString>& CookDirectories, const TArray<FString>& CookCultures, const TArray<FString>& IniMapSections );

	/**
	 * Queue a cook by the book cancel (you might want to do this instead of calling cancel directly so that you don't have to be in the game thread when canceling
	 */
	void QueueCancelCookByTheBook();

	/**
	 * Cancel the currently running cook by the book (needs to be called from the game thread)
	 */
	void CancelCookByTheBook();

	bool IsCookByTheBookRunning() const;

	/**
	 * Handles cook package requests until there are no more requests, then returns
	 *
	 * @param  CookFlags output of the things that might have happened in the cook on the side
	 * 
	 * @return returns ECookOnTheSideResult
	 */
	uint32 TickCookOnTheSide( const float TimeSlice, uint32 &CookedPackagesCount );
	
	/**
	 * Force stop whatever pending cook requests are going on and clear all the cooked data
	 * Note cook on the side / cook on the fly clients may not be able to recover from this if they are waiting on a cook request to complete
	 */
	void StopAndClearCookedData();

	/** 
	 * Process any shader recompile requests
	 */
	void TickRecompileShaderRequests();

	bool HasCookRequests() const { return CookRequests.HasItems(); }

	bool HasRecompileShaderRequests() const { return RecompileRequests.HasItems(); }


	uint32 NumConnections() const;

	
	/**
	 * Is this cooker running in real time mode (where it needs to respect the timeslice) 
	 * 
	 * @return returns true if this cooker is running in realtime mode like in the editor
	 */
	bool IsRealtimeMode();
	

	virtual void BeginDestroy() override;
	



	/**
	 * Callbacks from editor 
	 */

	void OnObjectModified( UObject *ObjectMoving );
	void OnObjectPropertyChanged(UObject* ObjectBeingModified, FPropertyChangedEvent& PropertyChangedEvent);
	void OnObjectUpdated( UObject *Object );

	/**
	 * Marks a package as dirty for cook
	 * causes package to be recooked on next request (and all dependent packages which are currently cooked)
	 */
	void MarkPackageDirtyForCooker( UPackage *Package );



private:
	
	//////////////////////////////////////////////////////////////////////////
	// cook by the book specific functions
	/**
	 * Collect all the files which need to be cooked for a cook by the book session
	 */
	void CollectFilesToCook(TArray<FString>& FilesInPath, 
		const TArray<FString>& CookMaps, const TArray<FString>& CookDirectories, const TArray<FString>& CookCultures, 
		const TArray<FString>& IniMapSections, bool bCookAll, bool bMapsOnly, bool bNoDev);
	/**
	 * Call back from the TickCookOnTheSide when a cook by the book finishes (when started form StartCookByTheBook)
	 */
	void CookByTheBookFinished();

	/**
	 * Helper function returns if we are in any cook by the book mode
	 *
	 * @return if the cook mode is a cook by the book mode
	 */
	inline bool IsCookByTheBookMode() const 
	{ 
		return CurrentCookMode == ECookMode::CookByTheBookFromTheEditor || CurrentCookMode == ECookMode::CookByTheBook; 
	}

	/**
	 * GetDependencies
	 * 
	 * @param Packages List of packages to use as the root set for dependency checking
	 * @param Found return value, all objects which package is dependent on
	 */
	void GetDependencies( const TSet<UPackage*>& Packages, TSet<UObject*>& Found);
	/**
	 * GenerateManifestInfo
	 * generate the manifest information for a given package
	 *
	 * @param Package package to generate manifest information for
	 */
	void GenerateManifestInfo( UPackage* Package, const TArray<FName>& TargetPlatformNames );

	//////////////////////////////////////////////////////////////////////////
	// cook on the fly specific functions
	/**
	 * Cook requests for a package from network
	 *  blocks until cook is complete
	 * 
	 * @param  Filename	requested file to cook
	 * @param  Platformname platform to cook for
	 * @param  out UnsolicitedFiles return a list of files which were cooked as a result of cooking the requested package
	 */
	void HandleNetworkFileServerFileRequest( const FString& Filename, const FString& Platformname, TArray<FString>& UnsolicitedFiles );

	/**
	 * Shader recompile request from network
	 *  blocks until shader recompile complete
	 *
	 * @param  RecompileData input params for shader compile and compiled shader output
	 */
	void HandleNetworkFileServerRecompileShaders(const struct FShaderRecompileData& RecompileData);

	//////////////////////////////////////////////////////////////////////////
	// general functions

	/**
	 * Determines if a package should be cooked
	 * 
	 * @param InFileName		package file name
	 * @param InPlatformName	desired platform to cook for
	 * @return If the package should be cooked
	 */
	bool ShouldCook(const FString& InFileName, const FName& InPlatformName);


	bool IsCookFlagSet( const ECookInitializationFlags& InCookFlags ) const 
	{
		return (CookFlags & InCookFlags) != ECookInitializationFlags::None;
	}

	/**
	 * Returns cooker output directory.
	 *
	 * @param PlatformName Target platform name.
	 * @return Cooker output directory for the specified platform.
	 */
	FString GetOutputDirectory( const FString& PlatformName ) const;

	/**
	 *	Get the given packages 'cooked' timestamp (i.e. account for dependencies)
	 *
	 *	@param	InFilename			The filename of the package
	 *	@param	OutDateTime			The timestamp the cooked file should have
	 *
	 *	@return	bool				true if the package timestamp was found, false if not
	 */
	bool GetPackageTimestamp( const FString& InFilename, FDateTime& OutDateTime );

	/**
	 *	Cook (save) the given package
	 *
	 *	@param	Package				The package to cook/save
	 *	@param	SaveFlags			The flags to pass to the SavePackage function
	 *	@param	bOutWasUpToDate		Upon return, if true then the cooked package was cached (up to date)
	 *
	 *	@return	bool			true if packages was cooked
	 */
	bool SaveCookedPackage( UPackage* Package, uint32 SaveFlags, bool& bOutWasUpToDate );
	/**
	 *	Cook (save) the given package
	 *
	 *	@param	Package				The package to cook/save
	 *	@param	SaveFlags			The flags to pass to the SavePackage function
	 *	@param	bOutWasUpToDate		Upon return, if true then the cooked package was cached (up to date)
	 *	@param  TargetPlatformNames Only cook for target platforms which are included in this array (if empty cook for all target platforms specified on commandline options)
	 *									TargetPlatformNames is in and out value returns the platforms which the SaveCookedPackage function saved for
	 *
	 *	@return	bool			true if packages was cooked
	 */
	bool SaveCookedPackage( UPackage* Package, uint32 SaveFlags, bool& bOutWasUpToDate, TArray<FName> &TargetPlatformNames );


	/**
	 *  Save the global shader map
	 *  
	 *  @param	Platforms		List of platforms to make global shader maps for
	 */
	void SaveGlobalShaderMapFiles(const TArray<ITargetPlatform*>& Platforms);


	/** Gets the output directory respecting any command line overrides */
	FString GetOutputDirectoryOverride( const FString& OutputDirectoryOverride ) const;

	/** Cleans sandbox folders for all target platforms */
	void CleanSandbox(const TArray<ITargetPlatform*>& Platforms);

	/** Generates asset registry */
	void GenerateAssetRegistry(const TArray<ITargetPlatform*>& Platforms);

	/** Generates long package names for all files to be cooked */
	void GenerateLongPackageNames(TArray<FString>& FilesInPath);


	void GetDependencies( UPackage* Package, TArray<UPackage*> Dependencies );

};

FORCEINLINE uint32 GetTypeHash(const UCookOnTheFlyServer::FFilePlatformRequest &Key)
{
	uint32 Hash = GetTypeHash( Key.Filename );
	for ( const auto &PlatformName : Key.Platformnames )
	{
		Hash += Hash << 2 ^ GetTypeHash( PlatformName );
	}
	return Hash;
}

