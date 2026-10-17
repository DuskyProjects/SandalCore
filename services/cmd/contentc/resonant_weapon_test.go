// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"aocrop/services/internal/designdb"
)

func resonantSource() *source {
	src := abilitySource()
	const root = abBase + 1000
	var refs list
	for i, name := range []string{"Resonance", "End_Cheerful", "End_Menacing", "End_Pensive", "End_Epic", "End_Cathartic", "DamagerCost"} {
		guid := root + int64(i)
		refs = append(refs, idv(guid, 6091248426050982904, "None"))
		ability, hit := int64(0), int64(4)
		trigger := fmt.Sprintf("HitHasTag(GetHitEvent(), \"Ability.weaponcombo\") && HitHasTag(GetHitEvent(), \"combostep.end\") && GetInstigator() == GetOwner() && GetEffectCount(GetInstigator(), $#3377240744094973029:%d$) == 1 && GetEffectCount(GetInstigator(), $#3377240744094973029:%d$) > 0", abEffPep, abResonance)
		if i == 0 {
			ability, hit = 4, 0
			trigger = "AbilityHasTag(GetConsumedAbility(), \"ability.Archetype\") && !AbilityHasTag(GetConsumedAbility(), \"ability.special\")"
		}
		if i == 6 {
			trigger = "HitHasTag(GetHitEvent(), \"combostep.end\") && GetInstigator() == GetOwner()"
		}
		targets := int64(1)
		if i == 2 || i == 5 {
			targets = 8
		}
		src.rows["ReactionRecordBase"] = append(src.rows["ReactionRecordBase"], mkRow(guid, "Bard_ResonantWeapon_"+name, st{
			"EventBindings": st{"AbilityEvents": ability, "HitEvents": hit}, "Instigator": "EReactionTarget::Owner", "TargetsBitmask": targets,
			"ReactTrigger": trigger, "ReactHitsIds": list{hitID(root + 100 + int64(i))},
		}))
		src.rows["AbilityHitRecordBase"] = append(src.rows["AbilityHitRecordBase"], hitRow(root+100+int64(i), "Resonant_"+name, nil))
	}
	for i := range src.rows["EffectRecordBase"] {
		row := &src.rows["EffectRecordBase"][i]
		if row.Guid == abEffPassive {
			row.Name = "Bard_ResonantWeapon"
			row.Fields["ReactionsIds"] = refs
			row.Fields["ExtensionAmount"] = "2"
			row.Fields["ExtendedMaximum"] = float64(15)
		}
	}
	src.rows["AbilityTargetingRecordBase"][0].Fields = st{"HurtBoxInfo": st{"TargetingVolumes": list{st{"SphereRadius": float64(3000)}}}, "Sorting": list{st{"Filter": "GetStat(GetTarget(), $#5365104778484440215:1$) < 100", "Score": "1"}}}
	for i := range src.rows["AbilityHitRecordBase"] {
		row := &src.rows["AbilityHitRecordBase"][i]
		if row.Guid == root+101 {
			row.Fields["RecursiveHit"] = list{st{"Hit": hitID(root + 200), "Targeting": targetID(abTarget)}}
		}
	}
	src.rows["AbilityHitRecordBase"] = append(src.rows["AbilityHitRecordBase"], hitRow(root+200, "Resonant_Recursive", st{"ProjectilesToFire": list{st{"Projectile": projectileID(root + 300)}}}), hitRow(root+201, "Resonant_Impact", st{"ApplyEffects": list{applyEffect(root+400, "1", "", "")}}))
	src.rows["EffectRecordBase"] = append(src.rows["EffectRecordBase"], effectRow(root+400, "Resonant_Nested_Effect", st{"TickHitsIds": list{hitID(root + 202)}}))
	src.rows["AbilityHitRecordBase"] = append(src.rows["AbilityHitRecordBase"], hitRow(root+202, "Resonant_Tick", st{"StatModsIds": list{modID(abModPep)}}))
	projectile := src.rows["ProjectileRecordBase"][0]
	projectile.Guid = root + 300
	projectile.Name = "Resonant_Projectile"
	projectile.Fields = st{}
	for k, v := range src.rows["ProjectileRecordBase"][0].Fields {
		projectile.Fields[k] = v
	}
	projectile.Fields["HitsIds"] = list{hitID(root + 201)}
	src.rows["ProjectileRecordBase"] = append(src.rows["ProjectileRecordBase"], projectile)
	completeFields(src)
	return src
}

func TestResonantReactionClosure(t *testing.T) {
	db, _, err := buildAbilitySynthetic(t, resonantSource())
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	for _, guid := range []int64{abBase + 1100, abBase + 1101, abBase + 1102, abBase + 1103, abBase + 1104, abBase + 1105, abBase + 1106, abBase + 1200, abBase + 1201, abBase + 1202} {
		if n := scalarInt(t, db, `SELECT count(*) FROM ab_hit WHERE guid=?`, guid); n != 1 {
			t.Errorf("reaction/recursive/projectile/effect hit %d not compiled", guid)
		}
	}
	var text string
	if err = db.SQL().QueryRow(`SELECT extra FROM ab_effect WHERE guid=?`, abEffPassive).Scan(&text); err != nil {
		t.Fatal(err)
	}
	var extra struct {
		ResonantWeaponRules []resonantWeaponRule
		ExtensionExpr       int64
		ExtendedMaximum     float64
	}
	if err = json.Unmarshal([]byte(text), &extra); err != nil {
		t.Fatal(err)
	}
	if len(extra.ResonantWeaponRules) != 7 || extra.ResonantWeaponRules[0].AbilityEvents != 4 || !extra.ResonantWeaponRules[6].DamagingCost || extra.ResonantWeaponRules[2].TargetsBitmask != 8 || extra.ExtensionExpr == 0 || extra.ExtendedMaximum != 15 {
		t.Fatalf("incorrect reaction/extension metadata: %+v", extra)
	}
	if err = db.SQL().QueryRow(`SELECT extra FROM ab_hit WHERE guid=?`, abBase+1101).Scan(&text); err != nil {
		t.Fatal(err)
	}
	var hit struct{ RecursiveHits []recursiveHit }
	if err = json.Unmarshal([]byte(text), &hit); err != nil {
		t.Fatal(err)
	}
	if len(hit.RecursiveHits) != 1 || hit.RecursiveHits[0].Targeting.Radius != 3000 || len(hit.RecursiveHits[0].Targeting.Sorting) != 1 || hit.RecursiveHits[0].Targeting.Sorting[0].FilterExpr == 0 {
		t.Fatalf("incorrect recursive targeting metadata: %+v", hit)
	}
}

func TestResonantReactionMissingRecord(t *testing.T) {
	src := resonantSource()
	src.rows["ReactionRecordBase"] = nil
	_, _, err := buildAbilitySynthetic(t, src)
	if err == nil || !strings.Contains(err.Error(), "missing reaction") {
		t.Fatalf("missing reaction accepted: %v", err)
	}
}

func TestRestoreAbilityRecordNumbersAndMaps(t *testing.T) {
	data := `{"Guid":6064632468947286527,"MaximumHoldTime":0,"HitsIds":[{"Guid":6064633430356270336,"Name":"None"}],"EffectsIds":[{"Key":1,"Value":{"Guid":6064632468947286527,"Name":"None"}}]}`
	decoder := json.NewDecoder(strings.NewReader(data))
	decoder.UseNumber()
	var value any
	if err := decoder.Decode(&value); err != nil {
		t.Fatal(err)
	}
	restored, err := restoreRecordValue(value, "")
	if err != nil {
		t.Fatal(err)
	}
	fields := restored.(designdb.Struct)
	if fields["Guid"] != int64(6064632468947286527) || fields["MaximumHoldTime"] != float64(0) {
		t.Fatalf("integer precision or float type lost: %+v", fields)
	}
	if _, ok := fields["HitsIds"].([]designdb.Value); !ok {
		t.Fatalf("projectile hit list mistaken for ability map: %T", fields["HitsIds"])
	}
	if _, ok := fields["EffectsIds"].(designdb.Pairs); !ok {
		t.Fatalf("ability effect map not restored: %T", fields["EffectsIds"])
	}
}

func TestRefreshAbilitiesPreservesExistingOutput(t *testing.T) {
	path := filepath.Join(t.TempDir(), "existing.db")
	want := []byte("existing content")
	if err := os.WriteFile(path, want, 0600); err != nil {
		t.Fatal(err)
	}
	err := refreshAbilities(t.Context(), "missing-input.db", path, nil)
	if err == nil || !strings.Contains(err.Error(), "already exists") {
		t.Fatalf("existing output accepted: %v", err)
	}
	got, err := os.ReadFile(path)
	if err != nil || string(got) != string(want) {
		t.Fatalf("existing output changed: %q, %v", got, err)
	}
}
