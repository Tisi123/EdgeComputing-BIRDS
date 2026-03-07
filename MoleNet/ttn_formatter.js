function decodeUplink(input) {
  var bytes = input.bytes;
  var data = {};
  
  // Species ID mapping
  var speciesMap = {
    1: "great_tit",
    2: "blue_tit",
    3: "robin",
    4: "blackbird",
    5: "sparrow",
    6: "woodpecker",
    7: "finch",
    8: "starling",
    254: "time_sync",      // Time synchronization message
    255: "unknown"
  };
  
  data.birds = [];
  data.count = 0;
  
  // Each entry is 6 bytes: species(1) + confidence(1) + timestamp(4)
  for (var i = 0; i < bytes.length; i += 6) {
    if (i + 5 < bytes.length) {
      var speciesId = bytes[i];
      var confidence = bytes[i + 1];
      var timestamp = (bytes[i + 2] << 24) | (bytes[i + 3] << 16) | (bytes[i + 4] << 8) | bytes[i + 5];
      
      var entry = {
        species: speciesMap[speciesId] || "unknown",
        confidence: confidence / 100.0,
        timestamp: timestamp,
        datetime: timestamp > 0 ? new Date(timestamp * 1000).toISOString() : "N/A"
      };
      
      // Add special flag for time sync messages
      if (speciesId === 254) {
        entry.message_type = "time_sync";
        entry.sync_successful = confidence === 100;
      }
      
      data.birds.push(entry);
      data.count++;
    }
  }
  
  return {
    data: data,
    warnings: [],
    errors: []
  };
}