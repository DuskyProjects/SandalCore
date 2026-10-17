// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"bytes"
	"context"
	"database/sql"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
)

// Refresh ability closures from their preserved design records. The input and
// any existing output are retained even if decoding or compilation fails.
func runRefreshAbilities(args []string, stdout, stderr io.Writer) int {
	fs := flag.NewFlagSet("contentc abilities", flag.ContinueOnError)
	fs.SetOutput(stderr)
	input := fs.String("content", filepath.Join(repoRoot(), "dist", "content", "content.db"), "existing content database containing preserved design records")
	output := fs.String("out", "", "new output database (required; must not exist)")
	if err := fs.Parse(args); err != nil {
		if err == flag.ErrHelp {
			return 0
		}
		return 2
	}
	if fs.NArg() != 0 || *output == "" {
		fmt.Fprintln(stderr, "contentc abilities: --out is required; use a new file")
		return 2
	}
	if err := refreshAbilities(context.Background(), *input, *output, stdout); err != nil {
		fmt.Fprintf(stderr, "contentc abilities: %v\n", err)
		return 1
	}
	fmt.Fprintf(stdout, "contentc abilities: wrote %s\n", *output)
	return 0
}

func refreshAbilities(ctx context.Context, input, output string, log io.Writer) error {
	if _, err := os.Stat(output); err == nil {
		return fmt.Errorf("output already exists: %s", output)
	} else if !os.IsNotExist(err) {
		return err
	}
	if st, err := os.Stat(input + "-wal"); err == nil && st.Size() != 0 {
		return fmt.Errorf("input has an active SQLite WAL; close/checkpoint its writer before refreshing")
	}
	db, err := content.Open(input)
	if err != nil {
		return err
	}
	src, err := abilitySourceFromRecords(db)
	closeErr := db.Close()
	if err != nil {
		return err
	}
	if closeErr != nil {
		return closeErr
	}
	in, err := os.Open(input)
	if err != nil {
		return err
	}
	defer in.Close()
	tmp, err := os.CreateTemp(filepath.Dir(output), ".ability-refresh-*.db")
	if err != nil {
		return err
	}
	tmpPath := tmp.Name()
	defer os.Remove(tmpPath)
	if _, err = io.Copy(tmp, in); err != nil {
		tmp.Close()
		return err
	}
	if err = tmp.Close(); err != nil {
		return err
	}
	sqlDB, err := sql.Open("sqlite", tmpPath)
	if err != nil {
		return err
	}
	defer sqlDB.Close()
	sqlDB.SetMaxOpenConns(1)
	tx, err := sqlDB.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	tableRows, err := tx.Query(`SELECT name FROM sqlite_master WHERE type='table' AND name GLOB 'ab_*' ORDER BY name`)
	if err != nil {
		return err
	}
	var tables []string
	for tableRows.Next() {
		var name string
		if err = tableRows.Scan(&name); err != nil {
			tableRows.Close()
			return err
		}
		tables = append(tables, name)
	}
	if err = tableRows.Err(); err != nil {
		tableRows.Close()
		return err
	}
	tableRows.Close()
	tables = append(tables, "loot_table", "xp_rule", "stat_program",
		"certification_level", "profession", "action_certification", "gather_dynamic", "gather_def", "gather_tool", "item_variation",
		"gather_interaction_def", "gather_interaction", "foliage_state", "foliage_def")
	for _, table := range tables {
		if _, err = tx.Exec(`DELETE FROM "` + strings.ReplaceAll(table, `"`, `""`) + `"`); err != nil {
			return err
		}
	}
	if _, err = tx.Exec(`DELETE FROM meta WHERE key GLOB 'ab.*' OR key GLOB 'loot.*' OR key GLOB 'gather.*' OR key GLOB 'statprog.*'`); err != nil {
		return err
	}
	w := &writer{tx: tx, stmts: map[string]*sql.Stmt{}}
	defer w.close()
	meta := map[string]string{content.MetaContentcVersion: Version, "ab.reactions.version": "1"}
	opt := Options{Log: log}
	if err = buildAbilities(w, src, &opt, meta); err != nil {
		return err
	}
	for key, value := range meta {
		if err = w.exec(`INSERT INTO meta(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value`, key, value); err != nil {
			return err
		}
	}
	if err = w.close(); err != nil {
		return err
	}
	if err = tx.Commit(); err != nil {
		return err
	}
	if err = addStatProgram(ctx, sqlDB, tmpPath, &opt); err != nil {
		return err
	}
	if err = sqlDB.Close(); err != nil {
		return err
	}
	// A hard link publishes the complete database atomically and refuses an
	// output created by another process after the initial existence check.
	return os.Link(tmpPath, output)
}

func abilitySourceFromRecords(db *content.DB) (*source, error) {
	meta, err := db.Meta()
	if err != nil {
		return nil, err
	}
	schemas := append([]string{
		"AoCAbilityRecordBase", "AbilityHitRecordBase", "EffectRecordBase", "StatModRecordBase",
		"ProjectileRecordBase", "LingeringEffectRecordBase", "AbilityTargetingRecordBase", "ReactionRecordBase",
		"StatTypeDefRecordBase", "SkillTreeRecordBase", "SkillRecordBase", "StatFormulaTypeRecordBase",
		"RewardTableRecordBase", "LevelingTypeDefRecordBase",
		"ActionResponseRecordBase", "ItemRecordBase", "GatheringFoliageStateRecordBase",
	}, gatherSchemas...)
	src := &source{rows: map[string][]designdb.Row{}, typeIDs: map[string]uint64{}}
	for _, schema := range schemas {
		rows, err := db.SQL().Query(`SELECT guid,parent_guid,name,dev_only,data FROM record WHERE schema=? ORDER BY guid`, schema)
		if err != nil {
			return nil, err
		}
		for rows.Next() {
			var row designdb.Row
			var encoded []byte
			if err = rows.Scan(&row.Guid, &row.ParentGuid, &row.Name, &row.DevOnly, &encoded); err != nil {
				rows.Close()
				return nil, err
			}
			data, err := content.DecodeData(meta[content.MetaRecordEncoding], encoded)
			if err != nil {
				rows.Close()
				return nil, err
			}
			decoder := json.NewDecoder(bytes.NewReader(data))
			decoder.UseNumber()
			var value any
			if err = decoder.Decode(&value); err != nil {
				rows.Close()
				return nil, err
			}
			var trailing any
			if err = decoder.Decode(&trailing); err != io.EOF {
				rows.Close()
				return nil, fmt.Errorf("%s %s: trailing or malformed record JSON", schema, row.Name)
			}
			restored, err := restoreRecordValue(value, "")
			if err != nil {
				rows.Close()
				return nil, fmt.Errorf("%s %s: %w", schema, row.Name, err)
			}
			var ok bool
			row.Fields, ok = restored.(designdb.Struct)
			if !ok {
				rows.Close()
				return nil, fmt.Errorf("%s %s: record is not an object", schema, row.Name)
			}
			if schema == "AoCAbilityRecordBase" || schema == "GatheringFoliageDataRecordBase" {
				for field, value := range row.Fields {
					for _, name := range []string{"EffectsIds", "HitsIds", "ProjectilesIds", "LingeringEffectsIds", "TargetingRecordsIds", "FoliageStateDataIds"} {
						if strings.EqualFold(field, name) {
							if values, ok := value.([]designdb.Value); ok && len(values) == 0 {
								row.Fields[field] = designdb.Pairs{}
							}
						}
					}
				}
			}
			src.rows[schema] = append(src.rows[schema], row)
		}
		if err = rows.Err(); err != nil {
			rows.Close()
			return nil, err
		}
		rows.Close()
	}
	return src, nil
}

// JSON preserves exact integer lexemes, but loses numeric property types and
// the distinction between a map's pair list and an ordinary array. Restore the
// specific types used by the ability compiler without rounding record GUIDs.
var abilityFloatFields = map[string]bool{"maximumholdtime": true, "areatargetrange": true, "areatargetradius": true, "networkingmaxspeed": true, "speed": true, "range": true, "lifetime": true, "chainradius": true, "spherecollisionradius": true, "tickrate": true, "delaytime": true, "ticktimer": true}

func restoreRecordValue(value any, field string) (designdb.Value, error) {
	switch v := value.(type) {
	case json.Number:
		isFloat := abilityFloatFields[strings.ToLower(field)]
		if !isFloat {
			if n, err := v.Int64(); err == nil {
				return n, nil
			}
		}
		if !isFloat && !strings.ContainsAny(string(v), ".eE") {
			n, err := strconv.ParseUint(string(v), 10, 64)
			if err != nil {
				return nil, err
			}
			return n, nil
		}
		return v.Float64()
	case map[string]any:
		if _, ok := v["History"]; ok {
			if _, ok = v["Namespace"]; ok {
				data, err := json.Marshal(v)
				if err != nil {
					return nil, err
				}
				var text designdb.Text
				if err = json.Unmarshal(data, &text); err != nil {
					return nil, err
				}
				return text, nil
			}
		}
		out := designdb.Struct{}
		for key, child := range v {
			restored, err := restoreRecordValue(child, key)
			if err != nil {
				return nil, err
			}
			out[key] = restored
		}
		return out, nil
	case []any:
		isPairs := false
		if len(v) > 0 {
			if first, ok := v[0].(map[string]any); ok {
				_, key := first["Key"]
				_, value := first["Value"]
				isPairs = key && value
			}
		}
		if isPairs {
			out := designdb.Pairs{}
			for _, child := range v {
				pair, ok := child.(map[string]any)
				if !ok {
					return nil, fmt.Errorf("%s is not a pair list", field)
				}
				key, err := restoreRecordValue(pair["Key"], "Key")
				if err != nil {
					return nil, err
				}
				item, err := restoreRecordValue(pair["Value"], "Value")
				if err != nil {
					return nil, err
				}
				out = append(out, designdb.Pair{Key: key, Value: item})
			}
			return out, nil
		}
		out := []designdb.Value{}
		for _, child := range v {
			restored, err := restoreRecordValue(child, "")
			if err != nil {
				return nil, err
			}
			out = append(out, restored)
		}
		return out, nil
	default:
		return value, nil
	}
}
