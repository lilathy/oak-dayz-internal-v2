void main()
{
	//INIT ECONOMY--------------------------------------
	Hive ce = CreateHive();
	if ( ce )
		ce.InitOffline();

	//DATE RESET AFTER ECONOMY INIT-------------------------
	int year, month, day, hour, minute;
	int reset_month = 9, reset_day = 20;
	GetGame().GetWorld().GetDate(year, month, day, hour, minute);

	if ((month == reset_month) && (day < reset_day))
	{
		GetGame().GetWorld().SetDate(year, reset_month, reset_day, hour, minute);
	}
	else
	{
		if ((month == reset_month + 1) && (day > reset_day))
		{
			GetGame().GetWorld().SetDate(year, reset_month, reset_day, hour, minute);
		}
		else
		{
			if ((month < reset_month) || (month > reset_month + 1))
			{
				GetGame().GetWorld().SetDate(year, reset_month, reset_day, hour, minute);
			}
		}
	}
}

class CustomMission: MissionServer
{
	float m_OakDoorAcc;
	float m_OakMeetAcc;
	bool m_OakMeetDidSpawnZeds;
	bool m_OakDidWorldProps;
	ref array<DayZInfected> m_OakTestZeds;

	void SetRandomHealth(EntityAI itemEnt)
	{
		if ( itemEnt )
		{
			float rndHlt = Math.RandomFloat( 0.45, 0.65 );
			itemEnt.SetHealth01( "", "", rndHlt );
		}
	}

	vector OakMeetPos(float sideOffset)
	{
		// Dual-client meet pad: NWAF hangar apron (dry concrete — old 4560/10240 sat in/near water).
		vector pos;
		pos[0] = 4790.0 + sideOffset;
		pos[2] = 10455.0;
		float sy = GetGame().SurfaceY(pos[0], pos[2]);
		// NWAF apron is ~339m. Old cap 250 forced Y=185 → 150m under the map.
		if (sy < 5.0 || sy > 450.0)
			sy = 339.0;
		// Reject sea/pond — SurfaceY can still look "valid" on water.
		if (GetGame().SurfaceIsSea(pos[0], pos[2]) || GetGame().SurfaceIsPond(pos[0], pos[2]))
		{
			pos[0] = 4685.0 + sideOffset;
			pos[2] = 10320.0;
			sy = GetGame().SurfaceY(pos[0], pos[2]);
			if (sy < 5.0 || sy > 450.0)
				sy = 339.0;
		}
		pos[1] = sy + 0.6;
		return pos;
	}

	void OakTeleportToMeet(PlayerBase player, float sideOffset)
	{
		if (!player)
			return;
		vector pos = OakMeetPos(sideOffset);
		player.SetPosition(pos);
		// Do not PlaceOnSurface here — SurfaceY is often 0 before terrain streams (under-map).
		vector after = player.GetPosition();
		bool wet = GetGame().SurfaceIsSea(after[0], after[2]) || GetGame().SurfaceIsPond(after[0], after[2]);
		if (after[1] < 5.0 || after[1] > 450.0 || wet)
		{
			after[0] = 4790.0 + sideOffset;
			after[2] = 10455.0;
			after[1] = 339.6;
			player.SetPosition(after);
		}
		Print("[OAK] meet TP player -> " + player.GetPosition().ToString());
	}

	// Last-logout / fresh spawn can sit under terrain until the navmesh streams.
	void OakFixUnderground(PlayerBase player)
	{
		if (!player)
			return;
		vector cur = player.GetPosition();
		float sy = GetGame().SurfaceY(cur[0], cur[2]);
		if (sy < 1.0 || sy > 400.0)
			return;
		if (GetGame().SurfaceIsSea(cur[0], cur[2]) || GetGame().SurfaceIsPond(cur[0], cur[2]))
			return;
		if (cur[1] + 0.2 >= sy)
			return;
		cur[1] = sy + 0.6;
		player.SetPosition(cur);
		Print("[OAK] lifted under-terrain -> " + cur.ToString());
	}

	void OakEnsureWorldProps(PlayerBase player)
	{
		if (m_OakDidWorldProps || !player)
			return;
		vector cur = player.GetPosition();
		float sy = GetGame().SurfaceY(cur[0], cur[2]);
		if (sy < 1.0 || sy > 400.0)
			return;
		m_OakDidWorldProps = true;
		SpawnOakTestBarrel(player);
		OakSpawnLockedTestBuilding(player);
		OakSpawnTestInfected(player);
	}

	void OakSpawnTestInfected(PlayerBase player)
	{
		if (m_OakMeetDidSpawnZeds || !player)
			return;
		m_OakMeetDidSpawnZeds = true;
		if (!m_OakTestZeds)
			m_OakTestZeds = new array<DayZInfected>;
		vector p = player.GetPosition();
		string types[2] = { "ZmbM_HermitSkinny_Beige", "ZmbM_SoldierNormal" };
		for (int i = 0; i < 2; i++)
		{
			vector zp = p;
			zp[0] = zp[0] + 2.5 + (i * 1.8);
			zp[2] = zp[2] + 3.0;
			float sy = GetGame().SurfaceY(zp[0], zp[2]);
			if (sy > 1.0 && sy < 450.0)
				zp[1] = sy + 0.7;
			else
				zp[1] = p[1] + 0.2;
			Object o = GetGame().CreateObjectEx(types[i], zp, ECE_PLACE_ON_SURFACE | ECE_INITAI);
			DayZInfected z = DayZInfected.Cast(o);
			if (z)
			{
				z.SetPosition(zp);
				if (i == 1)
					z.SetHealth01("", "Health", 0.40);
				m_OakTestZeds.Insert(z);
			}
		}
		Print("[OAK] test infected x" + m_OakTestZeds.Count().ToString() + " near player");
	}

	void OakEnsureTestZeds(PlayerBase player)
	{
		if (!player)
			return;
		if (m_OakTestZeds && m_OakTestZeds.Count() >= 2)
			return;
		m_OakMeetDidSpawnZeds = false;
		OakSpawnTestInfected(player);
	}

	void OakWriteZedVitals(FileHandle fh, DayZInfected z)
	{
		if (fh == 0 || !z)
			return;
		float z01 = z.GetHealth01("", "Health");
		if (z01 <= 0.001)
			return;
		vector zp = z.GetPosition();
		float zhp = z.GetHealth("", "Health");
		int zLo = 0;
		int zHi = 0;
		z.GetNetworkID(zLo, zHi);
		FPrintln(fh, "zed net=" + zLo.ToString() + " hp01=" + z01.ToString() + " hp=" + zhp.ToString() + " x=" + zp[0].ToString() + " y=" + zp[1].ToString() + " z=" + zp[2].ToString());
	}

	void OakSpawnMeetInfected()
	{
		if (m_OakMeetDidSpawnZeds)
			return;
		m_OakMeetDidSpawnZeds = true;

		vector base = OakMeetPos(0);
		string types[4] = { "ZmbM_HermitSkinny_Beige", "ZmbF_CitizenANormal_Blue", "ZmbM_SoldierNormal", "ZmbM_PatrolNormal_Autumn" };
		for (int i = 0; i < 4; i++)
		{
			vector zp = base;
			zp[0] = zp[0] + 4.0 + (i * 1.5);
			zp[2] = zp[2] + 3.0;
			zp[1] = GetGame().SurfaceY(zp[0], zp[2]);
			GetGame().CreateObjectEx(types[i], zp, ECE_PLACE_ON_SURFACE | ECE_INITAI);
		}
		Print("[OAK] meet infected x4 spawned near pad");
		// Dummy CreatePlayer(null) crashes 1.29 server — skip. Use dual-client / netsync offset hunt instead.
	}

	void OakGatherPlayersPulse()
	{
		array<Man> players = new array<Man>;
		GetGame().GetPlayers(players);
		if (players.Count() < 1)
			return;

		// Infected spawn gated off while diagnosing server exits after connect.
		// OakSpawnMeetInfected();

		for (int p = 0; p < players.Count(); p++)
		{
			PlayerBase player = PlayerBase.Cast(players.Get(p));
			if (!player)
				continue;
			float side = ((float)p - ((float)(players.Count() - 1) * 0.5)) * 2.5;
			vector want = OakMeetPos(side);
			vector cur = player.GetPosition();
			float dx = cur[0] - want[0];
			float dy = cur[1] - want[1];
			float dz = cur[2] - want[2];
			float dist2 = dx * dx + dz * dz;
			bool wet = false;
			if (player.IsSwimming() || player.IsInWater())
				wet = true;
			else if (GetGame().SurfaceIsSea(cur[0], cur[2]) || GetGame().SurfaceIsPond(cur[0], cur[2]))
				wet = true;
			// Re-snap if far, wrong height, or swimming / standing in water
			if (wet || dist2 > 625.0 || dy > 8.0 || dy < -8.0)
				OakTeleportToMeet(player, side);
		}

		// Server-side vitals census (authority): proves HP exists even when client DS is null.
		OakVitalsCensusPulse(players);
	}

	// Writes $profile:oak_vitals.tmp then copies over .txt so the client never reads a truncated file.
	void OakVitalsCensusPulse(array<Man> players)
	{
		FileHandle fh = OpenFile("$profile:oak_vitals.tmp", FileMode.WRITE);
		if (fh == 0)
			return;

		FPrintln(fh, "[OAKVITALS] t=" + GetGame().GetTime().ToString() + " n=" + players.Count().ToString());
		if (players.Count() > 0)
		{
			PlayerBase zHost = PlayerBase.Cast(players.Get(0));
			OakEnsureTestZeds(zHost);
		}
		for (int i = 0; i < players.Count(); i++)
		{
			PlayerBase pb = PlayerBase.Cast(players.Get(i));
			if (!pb)
				continue;
			float hp = pb.GetHealth("", "Health");
			float blood = pb.GetHealth("", "Blood");
			float shock = pb.GetHealth("", "Shock");
			float hp01 = pb.GetHealth01("", "Health");
			float energy = -1;
			float water = -1;
			float stam = -1;
			float e01 = -1;
			float w01 = -1;
			if (pb.GetStatEnergy())
				energy = pb.GetStatEnergy().Get();
			if (pb.GetStatWater())
				water = pb.GetStatWater().Get();
			if (pb.GetStatStamina())
				stam = pb.GetStatStamina().Get();
			string name = "unknown";
			if (pb.GetIdentity())
				name = pb.GetIdentity().GetName();
			// Fresh spawns sit at ~500/5000 energy+water (Food/Water HUD looks empty) and
			// Shock HP stays 100 until a shock hit. Seed once so the overlay is obviously live.
			if (energy >= 0 && energy < 800)
			{
				if (shock > 90)
					pb.SetHealth("", "Shock", 55);
				if (pb.GetStatEnergy())
					pb.GetStatEnergy().Set(2500);
				if (pb.GetStatWater())
					pb.GetStatWater().Set(3750);
				energy = 2500;
				water = 3750;
				shock = pb.GetHealth("", "Shock");
				Print("[OAKVITALS] seeded energy=2500 water=3750 shock=" + shock.ToString() + " " + name);
			}
			float eMax = PlayerConstants.SL_ENERGY_MAX;
			float wMax = PlayerConstants.SL_WATER_MAX;
			if (eMax > 1 && energy >= 0)
				e01 = energy / eMax;
			if (wMax > 1 && water >= 0)
				w01 = water / wMax;
			int hl = pb.m_HealthLevel;
			int shockS = pb.m_ShockSimplified;
			int bleed = pb.m_BleedingBits;
			FPrintln(fh, "player=" + name + " hp=" + hp.ToString() + " blood=" + blood.ToString() + " shock=" + shock.ToString() + " hp01=" + hp01.ToString() + " energy=" + energy.ToString() + " water=" + water.ToString() + " stam=" + stam.ToString() + " e01=" + e01.ToString() + " w01=" + w01.ToString() + " healthLevel=" + hl.ToString() + " shockS=" + shockS.ToString() + " bleed=" + bleed.ToString());
			int netLo = 0;
			int netHi = 0;
			pb.GetNetworkID(netLo, netHi);
			FPrintln(fh, "net=" + netLo.ToString() + ":" + netHi.ToString() + " hl=" + hl.ToString() + " hp01=" + hp01.ToString() + " hp=" + hp.ToString() + " blood=" + blood.ToString() + " shock=" + shock.ToString() + " bleed=" + bleed.ToString() + " energy=" + energy.ToString() + " water=" + water.ToString() + " stam=" + stam.ToString() + " e01=" + e01.ToString() + " w01=" + w01.ToString());
			Print("[OAKVITALS] " + name + " hp=" + hp.ToString() + " blood=" + blood.ToString() + " e=" + energy.ToString() + " w=" + water.ToString() + " stam=" + stam.ToString());

			// Force injury netsync — optional override file "$profile:oak_force_hl.txt" (single digit 0-4).
			int wantHl = -1;
			if (FileExist("$profile:oak_force_hl.txt"))
			{
				FileHandle fhHl = OpenFile("$profile:oak_force_hl.txt", FileMode.READ);
				if (fhHl != 0)
				{
					string line;
					if (FGets(fhHl, line) >= 0)
					{
						line = line.Trim();
						if (line.Length() > 0)
							wantHl = line.ToInt();
					}
					CloseFile(fhHl);
				}
			}
			if (wantHl < 0 && FileExist("$profile:oak_vitals_damage.on"))
			{
				wantHl = 0;
				if (hp01 < 0.1) wantHl = 4;
				else if (hp01 < 0.2) wantHl = 3;
				else if (hp01 < 0.3) wantHl = 2;
				else if (hp01 < 0.5) wantHl = 1;
			}
			if (wantHl >= 0 && wantHl <= 4 && pb.m_HealthLevel != wantHl)
			{
				pb.m_HealthLevel = wantHl;
				pb.SetSynchDirty();
				Print("[OAKVITALS] force hl=" + wantHl.ToString() + " for " + name);
			}
		}
		int zWritten = 0;
		if (m_OakTestZeds)
		{
			for (int zi = 0; zi < m_OakTestZeds.Count(); zi++)
			{
				DayZInfected z = m_OakTestZeds.Get(zi);
				if (!z)
					continue;
				vector zp = z.GetPosition();
				float sy = GetGame().SurfaceY(zp[0], zp[2]);
				if (sy > 1.0 && zp[1] < sy - 1.5)
				{
					zp[1] = sy + 0.7;
					z.SetPosition(zp);
				}
				OakWriteZedVitals(fh, z);
				zWritten = zWritten + 1;
			}
		}
		// Nearby world infected (HP only) — client matches by net or XZ position.
		if (players.Count() > 0 && zWritten < 32)
		{
			PlayerBase host = PlayerBase.Cast(players.Get(0));
			if (host)
			{
				array<Object> objs = new array<Object>;
				GetGame().GetObjectsAtPosition(host.GetPosition(), 250.0, objs, null);
				for (int oi = 0; oi < objs.Count() && zWritten < 32; oi++)
				{
					DayZInfected zz = DayZInfected.Cast(objs.Get(oi));
					if (!zz)
						continue;
					bool already = false;
					if (m_OakTestZeds)
					{
						for (int tj = 0; tj < m_OakTestZeds.Count(); tj++)
						{
							if (m_OakTestZeds.Get(tj) == zz)
							{
								already = true;
								break;
							}
						}
					}
					if (already)
						continue;
					OakWriteZedVitals(fh, zz);
					zWritten = zWritten + 1;
				}
			}
		}
		CloseFile(fh);
		// Overwrite in place — Delete+Copy made the client CreateFile hitch ~70ms.
		CopyFile("$profile:oak_vitals.tmp", "$profile:oak_vitals.txt");
		if (FileExist("$profile:oak_vitals.tmp"))
			DeleteFile("$profile:oak_vitals.tmp");
	}

	void GiveOakCombatKit(PlayerBase player)
	{
		if (!player)
			return;

		EntityAI gun = player.GetInventory().CreateInInventory("Aug");
		EntityAI mag1 = player.GetInventory().CreateInInventory("Mag_Aug_30Rnd");
		player.GetInventory().CreateInInventory("Mag_Aug_30Rnd");
		player.GetInventory().CreateInInventory("Mag_Aug_30Rnd");
		player.GetInventory().CreateInInventory("AmmoBox_556x45_20Rnd");

		EntityAI g1 = player.GetInventory().CreateInInventory("RGD5Grenade");
		player.GetInventory().CreateInInventory("RGD5Grenade");
		EntityAI g2 = player.GetInventory().CreateInInventory("M67Grenade");
		player.GetInventory().CreateInInventory("M67Grenade");

		player.GetInventory().CreateInInventory("PlateCarrierVest");
		player.GetInventory().CreateInInventory("AssaultBag_Ttsko");
		player.GetInventory().CreateInInventory("BallisticHelmet_Green");
		player.GetInventory().CreateInInventory("TacticalGloves_Black");

		if (gun)
			player.SetQuickBarEntityShortcut(gun, 0);
		if (mag1)
			player.SetQuickBarEntityShortcut(mag1, 1);
		if (g1)
			player.SetQuickBarEntityShortcut(g1, 2);
		if (g2)
			player.SetQuickBarEntityShortcut(g2, 3);
	}

	// T4-E1 - Lockpick x4. No teleport: keep last logout position.
	void GiveOakDoorKit(PlayerBase player)
	{
		if (!player)
			return;

		EntityAI pick = player.GetInventory().CreateInInventory("Lockpick");
		player.GetInventory().CreateInInventory("Lockpick");
		player.GetInventory().CreateInInventory("Lockpick");
		player.GetInventory().CreateInInventory("Lockpick");
		if (pick)
			player.SetQuickBarEntityShortcut(pick, 4);

		Print("[OAK] door kit: Lockpick x4 (QB slot 5)");
	}

	void SpawnOakTestBarrel(PlayerBase player)
	{
		if (!player)
			return;

		vector pos = player.GetPosition();
		vector dir = player.GetDirection();
		pos = pos + (dir * 2.5);
		pos[1] = GetGame().SurfaceY(pos[0], pos[2]);

		EntityAI barrel = EntityAI.Cast(GetGame().CreateObjectEx("Barrel_Red", pos, ECE_PLACE_ON_SURFACE));
		if (!barrel)
		{
			Print("[OAK] test barrel spawn FAILED");
			return;
		}

		Barrel_ColorBase barrelObj = Barrel_ColorBase.Cast(barrel);
		if (barrelObj)
			barrelObj.Open();

		barrel.GetInventory().CreateInInventory("BandageDressing");
		barrel.GetInventory().CreateInInventory("Apple");
		barrel.GetInventory().CreateInInventory("SodaCan_Cola");
		barrel.GetInventory().CreateInInventory("Ammo_556x45");
		barrel.GetInventory().CreateInInventory("Battery9V");
		barrel.GetInventory().CreateInInventory("Rag");
		barrel.GetInventory().CreateInInventory("TunaCan");
		barrel.GetInventory().CreateInInventory("SpaghettiCan");
		barrel.GetInventory().CreateInInventory("PowderedMilk");
		barrel.GetInventory().CreateInInventory("Matchbox");
		Print("[OAK] test barrel opened+spawned with loot at " + pos.ToString());
	}

	// Spawn a small garage ahead of the player and lock door 0 so the client
	// door-unlocker has a Building entity (map Land_* statics are often not
	// in Slow/Item lists).
	void OakSpawnLockedTestBuilding(PlayerBase player)
	{
		if (!player)
			return;

		vector pos = player.GetPosition();
		vector dir = player.GetDirection();
		pos = pos + (dir * 8.0);
		pos[1] = GetGame().SurfaceY(pos[0], pos[2]);

		Object obj = GetGame().CreateObjectEx("Land_Garage_Row_Small", pos, ECE_PLACE_ON_SURFACE);
		Building building;
		if (!Class.CastTo(building, obj))
		{
			Print("[OAK] test garage spawn FAILED at " + pos.ToString());
			OakLockNearestHouseDoor(player);
			return;
		}

		building.LockDoor(0, true);
		if (!building.IsDoorLocked(0))
			building.LockDoor(0, true);

		Print("[OAK] spawned locked Land_Garage_Row_Small door0 @ " + pos.ToString() + " locked=" + building.IsDoorLocked(0).ToString());
	}

	void OakLockNearestHouseDoor(PlayerBase player)
	{
		if (!player)
			return;

		vector pos = player.GetPosition();
		array<Object> nearby = new array<Object>;
		GetGame().GetObjectsAtPosition(pos, 40.0, nearby, null);

		for (int i = 0; i < nearby.Count(); i++)
		{
			Object o = nearby.Get(i);
			if (!o)
				continue;

			Building building;
			if (!Class.CastTo(building, o))
				continue;

			string t = o.GetType();
			if (t.Contains("Wreck") || t.Contains("Land_Misc") || t.Contains("Land_Mobile"))
				continue;

			for (int d = 0; d < 8; d++)
			{
				if (building.IsDoorLocked(d))
				{
					Print("[OAK] door already locked: " + t + " doorIndex=" + d.ToString());
					return;
				}
				building.LockDoor(d, true);
				if (building.IsDoorLocked(d))
				{
					Print("[OAK] pre-locked " + t + " doorIndex=" + d.ToString() + " @ player pos");
					return;
				}
			}
		}

		Print("[OAK] no nearby Building to pre-lock - lock one with Lockpick");
	}

	override PlayerBase CreateCharacter(PlayerIdentity identity, vector pos, ParamsReadContext ctx, string characterName)
	{
		Entity playerEnt;
		playerEnt = GetGame().CreatePlayer( identity, characterName, pos, 0, "NONE" );
		Class.CastTo( m_player, playerEnt );

		GetGame().SelectPlayer( identity, m_player );

		return m_player;
	}

	override void StartingEquipSetup(PlayerBase player, bool clothesChosen)
	{
		EntityAI itemClothing;
		EntityAI itemEnt;
		float rand;

		itemClothing = player.FindAttachmentBySlotName( "Body" );
		if ( itemClothing )
		{
			SetRandomHealth( itemClothing );
			
			itemEnt = itemClothing.GetInventory().CreateInInventory( "BandageDressing" );
			player.SetQuickBarEntityShortcut(itemEnt, 2);
			
			string chemlightArray[] = { "Chemlight_White", "Chemlight_Yellow", "Chemlight_Green", "Chemlight_Red" };
			int rndIndex = Math.RandomInt( 0, 4 );
			itemEnt = itemClothing.GetInventory().CreateInInventory( chemlightArray[rndIndex] );
			SetRandomHealth( itemEnt );
			player.SetQuickBarEntityShortcut(itemEnt, 1);

			rand = Math.RandomFloatInclusive( 0.0, 1.0 );
			if ( rand < 0.35 )
				itemEnt = player.GetInventory().CreateInInventory( "Apple" );
			else if ( rand > 0.65 )
				itemEnt = player.GetInventory().CreateInInventory( "Pear" );
			else
				itemEnt = player.GetInventory().CreateInInventory( "Plum" );
			player.SetQuickBarEntityShortcut(itemEnt, 3);
			SetRandomHealth( itemEnt );
		}
		
		itemClothing = player.FindAttachmentBySlotName( "Legs" );
		if ( itemClothing )
			SetRandomHealth( itemClothing );
		
		itemClothing = player.FindAttachmentBySlotName( "Feet" );

		GiveOakCombatKit(player);
		GiveOakDoorKit(player);
		// Barrel/garage wait until SurfaceY is real (OakEnsureWorldProps).
	}

	override void InvokeOnConnect(PlayerBase player, PlayerIdentity identity)
	{
		super.InvokeOnConnect(player, identity);
		GiveOakCombatKit(player);
		GiveOakDoorKit(player);
		// Default: last logout. Dual-client meet pad only if profiles\oak_meet_pad.on exists.
		if (FileExist("$profile:oak_meet_pad.on"))
		{
			float side = 0;
			array<Man> players = new array<Man>;
			GetGame().GetPlayers(players);
			side = ((float)(players.Count() - 1)) * 2.5;
			OakTeleportToMeet(player, side);
			Print("[OAK] player connect: kits + meet TP");
		}
		else
		{
			OakFixUnderground(player);
			Print("[OAK] player connect: kits + last logout (no meet TP)");
		}
		// Shock HP sits at 100 unless a hit lands. Dip once so the overlay bar is visibly live.
		if (player)
			player.AddHealth("", "Shock", -40);
	}

	// Dedicated server authority bridge: client UnlockDoor natives do not sync.
	// Client DLL writes oak_door_unlock.on into -profiles while Door Unlock is on.
	void OakDoorUnlockPulse()
	{
		if (!FileExist("$profile:oak_door_unlock.on"))
			return;

		array<Man> players = new array<Man>;
		GetGame().GetPlayers(players);
		for (int p = 0; p < players.Count(); p++)
		{
			PlayerBase player = PlayerBase.Cast(players.Get(p));
			if (!player)
				continue;

			vector pos = player.GetPosition();
			array<Object> nearby = new array<Object>;
			GetGame().GetObjectsAtPosition(pos, 15.0, nearby, null);

			for (int i = 0; i < nearby.Count(); i++)
			{
				Object obj = nearby.Get(i);
				Building building;
				if (!Class.CastTo(building, obj))
					continue;

				string t = obj.GetType();
				bool isTestGarage = t.Contains("Garage_Row_Small");

				for (int d = 0; d < 12; d++)
				{
					bool locked = building.IsDoorLocked(d);
					if (!locked && !isTestGarage)
						continue;
					if (locked)
						building.UnlockDoor(d);
					building.OpenDoor(d);
					Print("[OAK] server door unlock+open " + t + " #" + d.ToString() + " wasLocked=" + locked.ToString());
					if (isTestGarage)
						break;
				}
			}
		}
	}

	override void OnMissionStart()
	{
		super.OnMissionStart();
		m_OakDoorAcc = 0;
		m_OakMeetAcc = 0;
		m_OakMeetDidSpawnZeds = false;
		m_OakDidWorldProps = false;
		Print("[OAK] mission start: last-logout spawn, underground lift, door pulse");
	}

	override void OnUpdate(float timeslice)
	{
		super.OnUpdate(timeslice);

		m_OakDoorAcc = m_OakDoorAcc + timeslice;
		if (m_OakDoorAcc >= 0.5)
		{
			m_OakDoorAcc = 0;
			OakDoorUnlockPulse();
		}

		m_OakMeetAcc = m_OakMeetAcc + timeslice;
		if (m_OakMeetAcc >= 2.0)
		{
			m_OakMeetAcc = 0;
			array<Man> players = new array<Man>;
			GetGame().GetPlayers(players);
			if (players.Count() < 1)
				return;
			if (FileExist("$profile:oak_meet_pad.on"))
				OakGatherPlayersPulse();
			else
			{
				for (int i = 0; i < players.Count(); i++)
				{
					PlayerBase pb = PlayerBase.Cast(players.Get(i));
					OakFixUnderground(pb);
					OakEnsureWorldProps(pb);
				}
				OakVitalsCensusPulse(players);
			}
		}
	}
};

Mission CreateCustomMission(string path)
{
	return new CustomMission();
}
