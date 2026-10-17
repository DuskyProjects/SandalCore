// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"fmt"
	"regexp"
	"strconv"
	"strings"

	"aocrop/services/internal/designdb"
)

type resonantWeaponRule struct {
	Guid             int64
	MelodyEffectGuid int64
	HitGuid          int64
	AbilityEvents    int64
	HitEvents        int64
	TargetsBitmask   int64
	DamagingCost     bool
	Trigger          string
}

var melodyReactionEffect = regexp.MustCompile(`GetEffectCount\(GetInstigator\(\),\s*\$#\d+:(\d+)\$\)\s*==\s*1`)

func (b *abBuilder) resonantWeaponRules(effect *designdb.Row) ([]resonantWeaponRule, error) {
	f := newFields("Resonant Weapon", effect.Fields)
	var rules []resonantWeaponRule
	for _, guid := range refGuids(f, "ReactionsIds") {
		row := b.reactions[guid]
		if row == nil {
			return nil, fmt.Errorf("Resonant Weapon names missing reaction %d", guid)
		}
		rf := newFields("Reaction "+row.Name, row.Fields)
		bindings := newFields(rf.where+" bindings", rf.strukt("EventBindings"))
		hits := refGuids(rf, "ReactHitsIds")
		rule := resonantWeaponRule{Guid: guid, AbilityEvents: bindings.int("AbilityEvents"), HitEvents: bindings.int("HitEvents"), TargetsBitmask: rf.int("TargetsBitmask"), Trigger: rf.str("ReactTrigger")}
		if rf.str("Instigator") != "EReactionTarget::Owner" || len(hits) != 1 || (rule.AbilityEvents != 4 && rule.HitEvents != 4) {
			return nil, fmt.Errorf("reaction %s has unsupported Resonant Weapon bindings", row.Name)
		}
		rule.HitGuid = hits[0]
		rule.DamagingCost = strings.HasSuffix(row.Name, "_DamagerCost")
		if matches := melodyReactionEffect.FindStringSubmatch(rule.Trigger); len(matches) == 2 {
			var err error
			rule.MelodyEffectGuid, err = strconv.ParseInt(matches[1], 10, 64)
			if err != nil {
				return nil, err
			}
		} else if rule.AbilityEvents == 0 && !rule.DamagingCost {
			return nil, fmt.Errorf("reaction %s has no supported melody condition", row.Name)
		}
		if rf.err != nil {
			return nil, rf.err
		}
		if bindings.err != nil {
			return nil, bindings.err
		}
		rules = append(rules, rule)
	}
	return rules, f.err
}

type recursiveHit struct {
	HitGuid       int64
	TargetingGuid int64
	Targeting     *targetingMetadata `json:",omitempty"`
}

func recursiveHitRefs(f *fields) []recursiveHit {
	if _, ok := f.get("RecursiveHit"); !ok {
		return nil
	}
	var out []recursiveHit
	for i, value := range f.list("RecursiveHit") {
		s, ok := value.(designdb.Struct)
		if !ok {
			f.fail("RecursiveHit", "struct", value, true)
			return nil
		}
		rf := newFields(fmt.Sprintf("%s recursive[%d]", f.where, i), s)
		hit, _ := rf.id("Hit")
		target, _ := rf.id("Targeting")
		if rf.err != nil {
			f.err = rf.err
			return nil
		}
		if hit != 0 {
			out = append(out, recursiveHit{HitGuid: hit, TargetingGuid: target})
		}
	}
	return out
}

type targetingSort struct {
	Filter, Score         string
	FilterExpr, ScoreExpr int64
}
type targetingMetadata struct {
	Name    string
	Radius  float64
	Sorting []targetingSort
}

func (b *abBuilder) targetingMetadata(guid int64) (*targetingMetadata, error) {
	if guid == 0 {
		return nil, nil
	}
	row := b.targetingRecords[guid]
	if row == nil {
		return nil, fmt.Errorf("recursive hit names missing targeting %d", guid)
	}
	f := newFields("Targeting "+row.Name, row.Fields)
	out := &targetingMetadata{Name: row.Name}
	hurt := newFields(f.where+" hurtbox", f.strukt("HurtBoxInfo"))
	for _, value := range hurt.list("TargetingVolumes") {
		s, ok := value.(designdb.Struct)
		if !ok {
			return nil, fmt.Errorf("%s has invalid targeting volume", f.where)
		}
		volume := newFields(f.where+" volume", s)
		if _, ok := volume.get("SphereRadius"); ok {
			radius := volume.number("SphereRadius")
			if radius > out.Radius {
				out.Radius = radius
			}
		}
		if volume.err != nil {
			return nil, volume.err
		}
	}
	for _, value := range f.list("Sorting") {
		s, ok := value.(designdb.Struct)
		if !ok {
			return nil, fmt.Errorf("%s has invalid sorting", f.where)
		}
		sf := newFields(f.where+" sorting", s)
		sort := targetingSort{Filter: sf.str("Filter"), Score: sf.str("Score")}
		var err error
		sort.FilterExpr, err = b.exprs.id(sort.Filter, nil)
		if err != nil {
			return nil, err
		}
		sort.ScoreExpr, err = b.exprs.id(sort.Score, nil)
		if err != nil {
			return nil, err
		}
		if sf.err != nil {
			return nil, sf.err
		}
		out.Sorting = append(out.Sorting, sort)
	}
	if hurt.err != nil {
		return nil, hurt.err
	}
	return out, f.err
}
